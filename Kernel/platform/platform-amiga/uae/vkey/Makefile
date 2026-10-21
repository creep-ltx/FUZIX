vkey: vkey.c vk.c vk.h
	cc -O2 -Wall -o $@ vkey.c vk.c $$(pkg-config --cflags --libs wayland-client xkbcommon)

vk.h: virtual-keyboard-unstable-v1.xml
	wayland-scanner client-header $< $@
vk.c: virtual-keyboard-unstable-v1.xml
	wayland-scanner private-code $< $@

clean:
	rm -f vkey vk.c vk.h
