QT = core network
TEMPLATE = app
SOURCES += main.cpp mitm.cpp
HEADERS += mitm.h

win32:LIBS += -lws2_32

verbose {
  DEFINES += DEBUG
}

asan {
  QMAKE_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
  QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
  QMAKE_LFLAGS += -fsanitize=address,undefined
}
