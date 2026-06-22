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

## External SDKs / Headers

- This project is built against the xNVSE / NVSE plugin SDK and related modding
  headers available in the local development environment. Those upstream
  projects are not redistributed here as part of `itr-nvse`.
