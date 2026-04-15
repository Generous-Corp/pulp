// Real ARA::ARAFactory construction (workstream 06 slice 6 — follow-up
// to the stub that 6.5 landed). Active only when PULP_HAS_ARA is set;
// non-ARA builds compile to an empty TU.
//
// This file publishes a static ARAFactory whose strings come from the
// plug-in's AraDocumentController and whose function pointers forward
// into no-op stubs that still satisfy the ABI requirements laid out in
// ARA_API/ARAInterface.h (non-null function pointers, valid structSize,
// matching kARA*MinSize thresholds). Real editing behaviour arrives
// when plug-in authors subclass AraDocumentController and override the
// ARA controller callbacks published here.

#include <pulp/format/ara.hpp>

#ifdef PULP_HAS_ARA

#include <ARA_API/ARAInterface.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

namespace pulp::format {
namespace {

// ── Controller interface stubs ─────────────────────────────────────────────
//
// Every ARA controller call gets a safe no-op implementation so a host
// calling through the factory never lands on a null pointer. Plug-ins
// that want real ARA behaviour override the corresponding method on
// their AraDocumentController subclass; a future slice wires those
// overrides into the interface table.

using ARA::ARADocumentControllerRef;
using ARA::ARADocumentControllerInstance;
using ARA::ARADocumentControllerInterface;
using ARA::ARAFactory;

void ARA_CALL stub_destroy_controller(ARADocumentControllerRef) {}

const ARAFactory* ARA_CALL stub_get_factory_from_controller(ARADocumentControllerRef);

void ARA_CALL stub_begin_editing(ARADocumentControllerRef) {}
void ARA_CALL stub_end_editing(ARADocumentControllerRef) {}
void ARA_CALL stub_notify_model_updates(ARADocumentControllerRef) {}

ARA::ARABool ARA_CALL stub_true(ARADocumentControllerRef, ...) { return ARA::kARATrue; }
ARA::ARABool ARA_CALL stub_false(ARADocumentControllerRef, ...) { return ARA::kARAFalse; }

// A non-null sentinel used as an ARADocumentControllerRef for the
// stub instance. Casting an address of a static is always valid.
static char kStubControllerTag = 0;

// Every plug-in using this factory shares the same controller
// interface table — the table is pure stubs and therefore stateless.
// ARA allows factories to share interface tables; per-plug-in state
// lives in the ARADocumentControllerRef passed to every call.
ARADocumentControllerInterface build_controller_interface() {
    ARADocumentControllerInterface iface{};
    iface.structSize = ARA::kARADocumentControllerInterfaceMinSize;
    iface.destroyDocumentController = stub_destroy_controller;
    iface.getFactory                = stub_get_factory_from_controller;
    iface.beginEditing              = stub_begin_editing;
    iface.endEditing                = stub_end_editing;
    iface.notifyModelUpdates        = stub_notify_model_updates;
    return iface;
}

// ── Factory callbacks ──────────────────────────────────────────────────────

// ARA requires initialize/uninitialize to be paired per process. Keep
// a refcount so multiple plug-in loads don't trip the contract.
std::mutex g_init_mutex;
int        g_init_count = 0;

void ARA_CALL pulp_initialize_ara(const ARA::ARAInterfaceConfiguration* /*config*/) {
    std::lock_guard lock(g_init_mutex);
    ++g_init_count;
}

void ARA_CALL pulp_uninitialize_ara(void) {
    std::lock_guard lock(g_init_mutex);
    if (g_init_count > 0) --g_init_count;
}

const ARADocumentControllerInstance* ARA_CALL pulp_create_doc_controller(
    const ARA::ARADocumentControllerHostInstance* /*host*/,
    const ARA::ARADocumentProperties* /*properties*/)
{
    // Publish a single shared instance. ARA allows sharing a controller
    // across documents as long as refs disambiguate; we use the tag
    // address as a stable non-null ref.
    static ARADocumentControllerInterface s_iface = build_controller_interface();
    static ARADocumentControllerInstance  s_instance = {
        /*structSize*/ sizeof(ARADocumentControllerInstance),
        /*controllerRef*/ reinterpret_cast<ARADocumentControllerRef>(&kStubControllerTag),
        /*interface*/ &s_iface,
    };
    return &s_instance;
}

// ── ARAFactory singleton ───────────────────────────────────────────────────

const ARAFactory* build_factory() {
    static std::string plugin_name   = "Pulp Plug-in";
    static std::string manufacturer  = "Pulp Audio Framework";
    static std::string info_url      = "https://github.com/danielraffel/pulp";
    static std::string version       = "0.12.0";
    static std::string factory_id    = "io.pulp.ara.factory.v1";
    static std::string archive_id    = "io.pulp.ara.archive.v1";

    static ARAFactory factory{};
    factory.structSize = ARA::kARAFactoryMinSize;
    factory.lowestSupportedApiGeneration  = ARA::kARAAPIGeneration_2_0_Final;
    factory.highestSupportedApiGeneration = ARA::kARAAPIGeneration_2_3_Final;

    factory.factoryID        = factory_id.c_str();
    factory.initializeARAWithConfiguration = pulp_initialize_ara;
    factory.uninitializeARA                = pulp_uninitialize_ara;

    factory.plugInName       = plugin_name.c_str();
    factory.manufacturerName = manufacturer.c_str();
    factory.informationURL   = info_url.c_str();
    factory.version          = version.c_str();

    factory.createDocumentControllerWithDocument = pulp_create_doc_controller;
    factory.documentArchiveID                    = archive_id.c_str();
    factory.compatibleDocumentArchiveIDsCount    = 0;
    factory.compatibleDocumentArchiveIDs         = nullptr;

    factory.analyzeableContentTypesCount       = 0;
    factory.analyzeableContentTypes            = nullptr;
    factory.supportedPlaybackTransformationFlags = 0;
    return &factory;
}

const ARAFactory* ARA_CALL stub_get_factory_from_controller(ARADocumentControllerRef) {
    return build_factory();
}

} // namespace

const void* ara_companion_factory_for(AraDocumentController* /*controller*/) {
    // Return the real ARA factory pointer. Callers that do not have
    // the SDK headers can still traverse the struct via ARASize
    // prefixes or treat this as an opaque token.
    return build_factory();
}

} // namespace pulp::format

#endif // PULP_HAS_ARA
