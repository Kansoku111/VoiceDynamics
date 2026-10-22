# Third-party notices

VoiceDynamics uses the Steinberg VST3 SDK, version 3.8.0 build 66, commit `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`, under the MIT license. The SDK and applicable submodule license texts are retained in `licenses/`.

`licenses/cmake-LICENSE.txt` is the license of the SDK's `cmake` submodule (Steinberg build scripts). Kitware CMake 3.31.10 (BSD 3-Clause) is used as a build tool only and is neither linked nor redistributed. Evaluation scripts use NumPy, SciPy and python-soundfile (BSD licenses) and are not part of the plug-in binary.

The measurement follows ITU-R BS.1770-5 and EBU Tech 3341 / 3342 (K-weighting, gating, loudness range) and the True Peak method of BS.1770 Annex 2. The K-weighting coefficients are computed for any sample rate from the formulas as derived in libebur128 (MIT, Copyright (c) 2011 Jan Kokemuller); the formulas only, no code was copied. The compressor's soft knee follows D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor Design — A Tutorial and Analysis", JAES 60(6), 2012. All code is original project code; no third-party DSP library is linked.

VST is a trademark of Steinberg Media Technologies GmbH. VoiceDynamics is an independent project and is not affiliated with Steinberg, PreSonus/Fender or any maker of commercial audio products. No code, graphics, presets or measurements of commercial audio products are included.

VoiceDynamics itself is released under the MIT license (`LICENSE`). The manufacturer name shown to the host is `Kansoku111`, defined once in `src/version.h`.

The binaries are compiled with Microsoft Visual C++ 2022. The Microsoft C/C++ runtime is statically linked. Windows system components are provided by the user's operating system.
