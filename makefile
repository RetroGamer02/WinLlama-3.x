# OpenWatcom Win16 build for WinLlama.
# wcc is the 16-bit compiler.  wcl on Linux selects the 32-bit driver and
# links a DOS executable even when given -bt=windows.

APP     = winllama
CC      = wcc
CFLAGS  = -zW -ml -zq -w3 -i=$(%WATCOM)/h -i=$(%WATCOM)/h/win

all: $(APP).exe

$(APP).obj: $(APP).c $(APP).h
	$(CC) $(CFLAGS) -fo=$(APP).obj $(APP).c

$(APP).res: $(APP).rc $(APP).h
	wrc -q -r -i=$(%WATCOM)/h/win -fo=$(APP).res $(APP).rc

$(APP).exe: $(APP).obj $(APP).res
	wlink system windows name $(APP).exe file $(APP).obj option stack=16384 library windows.lib,winsock.lib
	wrc $(APP).res

clean: .SYMBOLIC
	-rm $(APP).obj
	-rm $(APP).res
	-rm $(APP).exe
	-rm $(APP).lnk
