
set(COMPONENT_SRCS "main.c")

set(COMPONENT_ADD_INCLUDEDIRS "")

# requirements can't depend on config
set(COMPONENT_REQUIRES mcommon mconfig mwifi mlink mupgrade mespnow)

register_component()
