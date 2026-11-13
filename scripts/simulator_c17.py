# GCC 15 compiles C as C23 by default; QRCode's `typedef unsigned char bool`
# is then an error. Pin C (not C++) to gnu17 for every library in the
# simulator build.
Import("env")
env.Append(CFLAGS=["-std=gnu17"])
