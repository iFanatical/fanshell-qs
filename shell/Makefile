# fanshell-qs-shell — hand-rolled C++/Qt bar for oxwm and qtile.
#
# Needs qtbase (widgets, gui, dbus, network) with the xcb platform plugin,
# libxcb (with RandR, DPMS and the screen saver extension, for displays,
# vibrance, screen power and lock on idle), libpulse, and
# X11/XComposite/XDamage/XFixes for the XEmbed tray.
#
# Fedora:  dnf install qt6-qtbase-devel pulseaudio-libs-devel libxcb-devel \
#                 libX11-devel libXcomposite-devel libXdamage-devel \
#                 libXfixes-devel
# Gentoo:  emerge dev-qt/qtbase media-libs/libpulse x11-libs/libxcb \
#                 x11-libs/libX11 x11-libs/libXcomposite x11-libs/libXdamage \
#                 x11-libs/libXfixes

QTMODS   = Qt6Widgets Qt6Gui Qt6Core Qt6DBus Qt6Network
PKGS     = $(QTMODS) libpulse xcb xcb-randr xcb-dpms xcb-screensaver x11 xcomposite xdamage xfixes

# XEmbed tray intake. Optional under Wayland in fanhypr-qs; on X11 it is the
# native tray protocol, so it is always built here.
override CXXFLAGS += -DFANSHELL_QS_HAVE_XEMBED

CXX      ?= g++
MOC      ?= $(shell pkg-config --variable=libexecdir Qt6Core)/moc
# `override` so `make CXXFLAGS=-g` (or LDLIBS=...) adds to the required flags
# instead of replacing them -- a plain += is discarded for command-line vars.
override CXXFLAGS += -std=c++17 -O2 -Wall -fPIC \
                     $(shell pkg-config --cflags $(PKGS))
override LDLIBS   += $(shell pkg-config --libs $(PKGS))

MODULES  = widgets popup surface procutil wmstate oxwmipc oxwmstate qtileipc \
           qtilestate calendar pills \
           panel network bluetooth volume tray traymenu desktopentry \
           launcher ipc clipboard system battery wallpaper notification osd \
           hardwarecontrols powerprofiles media menu xembedtray vibrance displays \
           controlpanel keyboard
# headers containing Q_OBJECT classes
MOCHDRS  = widgets popup procutil wmstate oxwmipc oxwmstate qtilestate calendar \
           pills panel \
           network bluetooth volume tray traymenu desktopentry launcher ipc \
           clipboard system battery wallpaper notification osd \
           hardwarecontrols powerprofiles media xembedtray vibrance displays

OBJ      = $(MODULES:%=%.o) main.o $(MOCHDRS:%=moc_%.o)

all: fanshell-qs-shell

fanshell-qs-shell: $(OBJ)
	$(CXX) -o $@ $(OBJ) $(LDLIBS)

moc_%.cpp: %.h
	$(MOC) $< -o $@

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJ): $(wildcard *.h)

clean:
	rm -f fanshell-qs-shell *.o moc_*.cpp

.PHONY: all clean
