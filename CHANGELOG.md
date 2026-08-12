# Changelog

## [1.1.0](https://github.com/jonocairns/coaxn/compare/v1.0.0...v1.1.0) (2026-08-12)


### Features

* harden live recovery and refresh the Coax mark ([#18](https://github.com/jonocairns/coaxn/issues/18)) ([bc5405c](https://github.com/jonocairns/coaxn/commit/bc5405c0e49841e11090ca7425beae211c15dff8))
* **ui:** neutral theme, new mark, reworked channel list ([#9](https://github.com/jonocairns/coaxn/issues/9)) ([9ee75ef](https://github.com/jonocairns/coaxn/commit/9ee75ef61d8732d47ecd23bcbe22f7af7f29ee50))


### Bug Fixes

* hold unity speed while live playback stalls ([#22](https://github.com/jonocairns/coaxn/issues/22)) ([0c214d6](https://github.com/jonocairns/coaxn/commit/0c214d61fa4ac6fab61f3f713c4d39095121a52e))
* keep live playback awake through recovery ([#20](https://github.com/jonocairns/coaxn/issues/20)) ([b206b6e](https://github.com/jonocairns/coaxn/commit/b206b6eb56f5d6c11bc17597966c95f0c417c270))
* make advancing replay observable ([#17](https://github.com/jonocairns/coaxn/issues/17)) ([a387600](https://github.com/jonocairns/coaxn/commit/a387600ea0406fc66c8b9bf237221fa049dacd36))
* make presentation failure recoverable and terminal instead of an endless spin (findings 7 and 9) ([#15](https://github.com/jonocairns/coaxn/issues/15)) ([d672a45](https://github.com/jonocairns/coaxn/commit/d672a454ca1190761373c987d67f8aa262a1ce2c))
* run the portable player tests in CI, and fix three live-sync latency defects ([#13](https://github.com/jonocairns/coaxn/issues/13)) ([1d2cc24](https://github.com/jonocairns/coaxn/commit/1d2cc24542b868d507d0c53b89e72386acba0f6d))
* stop charging rebuffers for mpv's opening fill, and make Steady mean what it claims ([#16](https://github.com/jonocairns/coaxn/issues/16)) ([ea1f579](https://github.com/jonocairns/coaxn/commit/ea1f5791e05bd11049bd60dbb6a570b9deae7a92))
* verify the libmpv archive, and close the two P1 logging defects ([#14](https://github.com/jonocairns/coaxn/issues/14)) ([1d1b5ef](https://github.com/jonocairns/coaxn/commit/1d1b5ef904669845cc36888ee09ac2bc6d4c6f77))

## 1.0.0 (2026-08-04)


### Features

* establish native player baseline ([34bd589](https://github.com/jonocairns/coaxn/commit/34bd589c2e7888c3387c68ccf417bcf6a04eca83))
* own the composition swap chain's lifetime and survive device loss ([3097f8a](https://github.com/jonocairns/coaxn/commit/3097f8ab53efd1b80fc1d4188883bfa77c16d14e))
* package releases and check for updates ([45acc0b](https://github.com/jonocairns/coaxn/commit/45acc0bd404a1abf397b481982bd0facbf926df6))
* port playback supervisor to native core ([2e15db2](https://github.com/jonocairns/coaxn/commit/2e15db2d4355ce83b80285e5ac785349dfaa39ee))
* rebuild the playback overlay as frameless controls ([0fba198](https://github.com/jonocairns/coaxn/commit/0fba19843f10093bbc96ef1efd55a82816306eec))
* theme the interface and make scaling DPI-correct ([186cca4](https://github.com/jonocairns/coaxn/commit/186cca4a0abea8af5789d1ec1a78ee56410a4f58))


### Bug Fixes

* do not draw or report against a half-rebuilt presentation ([823a364](https://github.com/jonocairns/coaxn/commit/823a364bd7636ed065aeda2d74685c46b4bda8d8))
* keep portal credentials out of the session log ([2214c54](https://github.com/jonocairns/coaxn/commit/2214c541be7de8824a8f4acd731eddb6681731cd))
* keep portal credentials out of the session log ([86f18b7](https://github.com/jonocairns/coaxn/commit/86f18b726e798d0a06058d80c77c9e54de2e6008))
