# CHANGELOG
All notable changes to this package will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](http://semver.org/spec/v2.0.0.html).

# [0.2.0] - 2026-10-08
This version requires updated character and language modules, please re-download your characters. Modules made for earlier package versions can no longer be imported.

### Added
* Numbers in the input text are now spoken, including ordinals such as "21st". Audio sample requests placed inside a number keep their position in the spoken result.
### Changed
* How text is read - normalization, how numbers are spoken and what counts as a word - now comes from the language module, so it can be improved with a module update instead of a new library version.
* Each segment is normalized with the rules of its own language, so text that mixes languages is handled according to each language.
### Removed
* The built-in Unicode tables and `tools/gen_unicode_tables.py`. The Unicode data now ships inside each language module.

# [0.1.0] - 2026-10-02

Initial release