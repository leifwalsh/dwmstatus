CPPFLAGS += -Ihostap/src/common -Ihostap/src/utils
CXXFLAGS += -std=c++11 -g -O2 -Wall -Wextra
LDLIBS += -lX11 -lasound

prefix=$(HOME)

all: dwmstatus

dwmstatus: dwmstatus.cpp hostap/src/common/wpa_ctrl.o hostap/src/utils/libutils.a

hostap/src/common/wpa_ctrl.o: hostap/src/common/wpa_ctrl.c hostap/wpa_supplicant/.config
	$(MAKE) -C hostap/wpa_supplicant ../src/common/wpa_ctrl.o

hostap/wpa_supplicant/.config: hostap/wpa_supplicant/defconfig
	cp $< $@

hostap/src/utils/libutils.a:
	$(MAKE) -C $(dir $@) $(notdir $@)

install: dwmstatus
	install -m 0755 dwmstatus $(prefix)/bin

clean:
	$(RM) dwmstatus hostap/src/common/wpa_ctrl.o hostap/wpa_supplicant/.config
	$(MAKE) -C hostap/src/utils clean
