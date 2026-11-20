# Choosing a Pulp UI

These recipes select the UI dependency set during setup. The default is
Vellum; `none` keeps a project audio-only.

## Default Vellum plug-in

From a fresh checkout on Apple Silicon macOS, run:

```sh
./setup.sh
pulp create "My Effect" --ui vellum
pulp build
```

`setup.sh` reads the Vellum SDK asset URL and digest from `pulp.lock` and
stages the verified SDK. Other platforms continue with an audio-only setup
until a Vellum SDK asset is published for that platform.

## DSP only

To avoid the Vellum, Skia, Dawn and RmlUi dependency path:

```sh
PULP_UI=none ./setup.sh --deps-only
pulp create "My Effect" --ui none
pulp build
```

The `--ui none` project has no editor and contains only the DSP/plugin formats.

## RmlUi

There is no `pulp create --ui rmlui` editor package yet. RmlUi currently lives
in Vellum's standalone `rmlui/` module and Pulp's native adapter is
`integrations/rmlui-native`. Build the native surface and point CMake at an
installed Vellum SDK that includes `VellumRmlUi::Native`:

```sh
./setup.sh
cmake -S . -B build -DPULP_ENABLE_PULP_NATIVE=ON \
  -DPULP_ENABLE_RMLUI_ADAPTER=ON \
  -DCMAKE_PREFIX_PATH=/path/to/vellum/install
pulp build
```

This path is for products that already use RML/RCSS skins or Lua skin scripts;
RmlUi remains opt-in and is not fetched by the default Vellum setup.
