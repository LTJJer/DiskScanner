# to-do:




# 项目配置
TARGET = "Disk Scanner"

TEMPLATE = app



CONFIG += c++23



QT += widgets concurrent



# 源文件
HEADERS += \
    $$files(*.h) \
    $$files(*.hpp)

SOURCES += \
    $$files(*.c) \
    $$files(*.cpp)

FORMS += \
    $$files(*.ui)

RESOURCES += \
    $$files(*.qrc)

OTHER_FILES += \
    LICENSE \
    README.md \
    # index.html



# 应用信息
RC_ICONS = ./Resources/Icon/icon.ico

VERSION = 1.4.0
MAKE_TARGET_COMPANY = "LT_JJ"
QMAKE_TARGET_DESCRIPTION = $${TARGET}
QMAKE_TARGET_COPYRIGHT = "Copyright © 2026 LT_JJ. Licensed under MIT."



# 环境配置
DEFINES += APP_VERSION=\\\"$${VERSION}\\\"

CONFIG(debug, debug|release) {
    BUILD_TYPE = "Debug"
} else {
    BUILD_TYPE = "Release"
}

DESTDIR = $${PWD}/bin/$${BUILD_TYPE}



# 编译优化
QMAKE_CXXFLAGS_RELEASE += -O3 -flto=thin
QMAKE_LFLAGS_RELEASE += -O3 -flto=thin



# 部署
win32
{
    EXE_NAME = $${TARGET}.exe
    BUILD_EXE = $${DESTDIR}/$${EXE_NAME}

    DEPLOY_EXE = D:/$${TARGET}/$${EXE_NAME}

    WINDEPLOYQT = $$[QT_INSTALL_LIBEXECS]/windeployqt.exe

    BUILD_LICENSE = $${PWD}/LICENSE.txt
    BUILD_README = $${PWD}/README.md



    # Args: ^<BUILD_EXE^> ^<DEPLOY_EXE^> ^<BUILD_README^> ^<BUILD_LICENSE^> ^<WINDEPLOYQT^> ^<DEPLOY_ARGS^> ^<DO_ARCHIVE^>

    deploy.commands = E:/Qt/Tools/deploy.bat             \
                    \"$$system_path($${BUILD_EXE})\"     \  # BUILD_EXE
                    \"$$system_path($${DEPLOY_EXE})\"    \  # DEPLOY_EXE
                    \"\"  \  # BUILD_README
                    \"$$system_path($${BUILD_LICENSE})\" \  # BUILD_LICENSE
                    \"$$system_path($${WINDEPLOYQT})\"   \  # WINDEPLOYQT
                    \"--no-translations --no-system-d3d-compiler --no-opengl-sw -no-svg -no-network --skip-plugin-types imageformats\" \  # DEPLOY_ARGS
                    \"1\"  # DO_ARCHIVE

    QMAKE_EXTRA_TARGETS += deploy
}
