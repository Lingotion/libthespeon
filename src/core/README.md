# Core layout

`core` is one library with source-level ownership boundaries:

- `module/` owns installed-module types, configuration parsing, and catalogs.
- `input/` owns synthesis input types and their JSON representation.
- `inference/` owns text and prosody preprocessing, input building, execution,
  caches, and streaming policy.
- `package/` owns pack import and installed-module removal.
- `utils/` contains isolated leaf utilities, currently only WAV serialization.
- `engine.*` is the facade that coordinates those areas.

Dependencies point toward `module/`: input may use module value types;
inference may use input and module; package may use module; and the engine may
coordinate every area. Lower-level areas do not include `engine.h`, and package
code does not depend on inference code.
