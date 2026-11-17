# Third-party notices

## NVEnc colorspace algorithms

`src/core/colorspace.cpp` and `src/apple/shaders/colorspace.metal` adapt matrix,
white-point adaptation, transfer functions, HDR2SDR tone-mapping curves and
highlight desaturation algorithms from
[rigaya/NVEnc](https://github.com/rigaya/NVEnc), revision
`5bb35d860bc5f877b01fe60e4de5ac756b9b80ed`, specifically
`NVEncCore/NVEncFilterColorspace.cpp` and `NVEncCore/NVEncFilterColorspaceFunc.h`.
The implementation is ported to C++20 / Metal; no CUDA runtime is included.

The MIT License

Copyright (c) 2014-2016 rigaya

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## zimg transfer functions

NVEnc identifies the transfer functions as modified from
[zimg's colorspace/gamma.cpp](https://github.com/sekrit-twc/zimg/blob/master/src/zimg/colorspace/gamma.cpp).
zimg is distributed under the following license (upstream `COPYING`):

```
            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
                    Version 2, December 2004

 Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>

 Everyone is permitted to copy and distribute verbatim or modified
 copies of this license document, and changing it is allowed as long
 as the name is changed.

            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

  0. You just DO WHAT THE FUCK YOU WANT TO.

```
