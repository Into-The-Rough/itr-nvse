# Third-Party Notes

`itr-nvse` is licensed under the MIT license. Included third-party code and code
origins are documented here for clarity.

## Bundled Third-Party Code

- [`itr-nvse/PerlinNoise.hpp`](itr-nvse/PerlinNoise.hpp)
  is `siv::PerlinNoise` by Ryo Suzuki. The file carries its own MIT license
  notice, which is retained in place.

## Code Origins

- `VATSSpeechFix` (prevents voice/dialogue audio from slowing during VATS) is
  ported directly from SoundFilteringSoftware.

- `RefillAmmo` (commands/ImperativeCommands.cpp) is based on `RefillPlayerAmmo`
  from ShowOff-NVSE by Demorome, generalized to any actor. ShowOff-NVSE permits
  code reuse with credit. https://github.com/Demorome/ShowOff-NVSE

## Bundled NVSE SDK

`NVSE/nvse` and `common` are the modified NVSE 5.1.4 SDK snapshot itr-nvse
builds against, by Ian Patterson, Stephen Abel and Paul Connelly. Bundled so
the project builds from a clean checkout. A stock NVSE or xNVSE SDK will not
work in its place. `common/common_license.txt` is its own licence, retained.
