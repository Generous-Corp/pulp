#include "portmap_merge.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
static int bad = 0;
static void ok(const char* m) { printf("  ok     %s\n", m); }
static void wrong(const char* m) { printf("  WRONG  %s\n", m); ++bad; }
static std::string mod(const char* plug, const char* model) {
    return std::string("    {\n      \"plugin\": \"") + plug +
           "\",\n      \"model\": \"" + model + "\",\n      \"params\": []\n    }";
}
/// A module block exactly as the CURRENT scanner writes one: scan 3, controls
/// carrying a kind, and lights and displays that no earlier map has.
///
/// The merge is TEXTUAL — module blocks are found by indentation — so a shape
/// with nested arrays inside a module is the case it has never seen. Getting
/// this wrong on the first real scan would take the whole map with it.
static std::string mod_v3(const char* plug, const char* model) {
    return std::string("    {\n      \"plugin\": \"") + plug +
           "\",\n      \"model\": \"" + model +
           "\",\n      \"pluginVersion\": \"2.6.4\""
           ",\n      \"scan\": 3"
           ",\n      \"size\": [135.000000, 380.000000]"
           ",\n      \"params\": ["
           "\n        {\"index\": 0, \"name\": \"Freq\", \"x\": 30.0,"
           " \"y\": 90.0, \"w\": 20.0, \"h\": 20.0, \"kind\": \"knob\"}"
           "\n      ]"
           ",\n      \"inputs\": ["
           "\n        {\"index\": 0, \"name\": \"FM\", \"x\": 20.0, \"y\": 286.0}"
           "\n      ]"
           ",\n      \"outputs\": []"
           ",\n      \"lights\": ["
           "\n        {\"x\": 60.0, \"y\": 40.0, \"w\": 6.0, \"h\": 6.0},"
           "\n        {\"x\": 75.0, \"y\": 40.0, \"w\": 6.0, \"h\": 6.0}"
           "\n      ]"
           ",\n      \"displays\": ["
           "\n        {\"x\": 67.0, \"y\": 200.0, \"w\": 100.0, \"h\": 30.0}"
           "\n      ]"
           "\n    }";
}
static std::string doc(const std::string& body) {
    return "{\n  \"modules\": [\n" + body + "\n  ]\n}\n";
}
int main() {
    using namespace forge_portmap;

    // A second batch must ADD, not replace. This is the whole point: no screen
    // holds a library, so a library is mapped in batches, and the previous
    // behaviour erased each batch with the next.
    const auto first = doc(mod("Fundamental", "VCO"));
    const auto second = doc(mod("Fundamental", "VCF"));
    const auto both = merge(first, second);
    auto keys = module_keys(both);
    if (keys.count("Fundamental/VCO") && keys.count("Fundamental/VCF"))
        ok("a second batch adds to the first");
    else
        wrong("a second batch lost the first — batching a library is impossible");

    // Re-measuring a module takes the NEW reading, and only one of it.
    const auto again = merge(both, doc(mod("Fundamental", "VCO")));
    if (module_keys(again).size() == 2) ok("a re-measured module is not duplicated");
    else wrong("re-measuring duplicated a module");

    // An empty scan must not blank the map. Pressing SCAN on a rack holding
    // only the scanner would otherwise destroy everything mapped so far.
    const auto empty = merge(both, doc(""));
    if (module_keys(empty).size() == 2) ok("an empty scan keeps what was mapped");
    else wrong("an empty scan wiped the map");

    // Anything unexpected leaves the fresh scan alone rather than producing a
    // file that is neither the old map nor the new one.
    if (merge("this is not a port map at all", second) == second)
        ok("an unreadable old map is replaced, not spliced");
    else
        wrong("an unreadable old map produced something else again");

    // And against the REAL file, because a fixture I wrote agrees with the
    // parser I wrote. This one was written by the scanner in Rack.
    std::ifstream f(std::string(std::getenv("HOME")) +
                    "/Library/Application Support/Rack2/forge-portmap.json");
    if (f) {
        std::stringstream ss; ss << f.rdbuf();
        const auto real = ss.str();
        const auto n = module_keys(real).size();
        if (n >= 19) {
            printf("  ok     the real map parses: %zu modules\n", n);
            const auto grown = merge(real, doc(mod("Vendor", "Newly")));
            if (module_keys(grown).size() == n + 1)
                ok("merging into the real map keeps every module and adds one");
            else
                wrong("merging into the real map lost modules");
        } else {
            printf("  WRONG  the real map parsed as %zu modules, expected >= 19\n", n);
            ++bad;
        }
    } else {
        printf("  skip   no real port map on this machine (a skip, not a pass)\n");
    }
    // The shape the next real SCAN will produce, merged into a map of the
    // shape every existing one has. Nested arrays inside a module block are
    // the case the textual merge has never seen, and the first scan after
    // tonight is when it meets them.
    {
        const auto old_map = doc(mod("Vendor", "Kept") + ",\n" +
                                 mod("Vendor", "Replaced"));
        const auto scan3 = doc(mod_v3("Vendor", "Replaced") + ",\n" +
                               mod_v3("Vendor", "Added"));
        const auto merged = merge(old_map, scan3);
        const auto keys = module_keys(merged);
        if (keys.size() == 3 && keys.count("Vendor/Kept") &&
            keys.count("Vendor/Replaced") && keys.count("Vendor/Added"))
            ok("a scan carrying lights and displays merges without loss");
        else {
            wrong("a v3 scan broke the merge");
            printf("         got %zu module(s)\n", keys.size());
        }
        // The re-measured one must come from the FRESH scan, so its new
        // fields are the ones kept.
        if (merged.find("\"lights\"") != std::string::npos &&
            merged.find("\"scan\": 3") != std::string::npos)
            ok("the re-measured module keeps its new fields");
        else
            wrong("the merge dropped the fields the new scan added");
    }

    printf("\n%s\n", bad ? "FAILED" : "all good");
    return bad ? 1 : 0;
}
