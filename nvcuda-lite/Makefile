CC ?= x86_64-w64-mingw32-gcc
all: nvcuda.dll
nvcuda.dll: nvcuda_lite.c
	$(CC) -O2 -shared -static -s -o $@ $< -Wl,--kill-at
	python3 stamp_builtin.py $@
clean:
	rm -f nvcuda.dll
.PHONY: all clean
