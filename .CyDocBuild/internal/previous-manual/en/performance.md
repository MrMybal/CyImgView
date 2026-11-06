---
id: performance
title: Measure performance
description: Measure performance — CyImgView.
section: Developers
order: 230
revision: 1
---

## Measure performance {#performance}

```bat
build\Release\bin\cyimg_bench.exe D:\Photos\MyFolder
build\Release\bin\cyimg_bench.exe D:\Photos\MyFolder 160 best
```

This displays full-resolution/thumbnail decode times and EXIF. With `best`, it also measures
full RAW development even when the preview is sufficient.

Command-line conversion, with output format inferred from the extension:

```bat
build\Release\bin\cyimg_bench.exe convert photo.CR3 photo.jpg 90
```

With `CYIMG_TRACE=1` in the environment, viewer startup is timed in `%TEMP%\cyimg_trace.txt`.


