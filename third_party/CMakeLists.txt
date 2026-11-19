# The vendored layout stack (README.md here): static libraries, built from
# the files in this tree only, no network. Upstream code compiles with its
# warnings off, and its include directories are SYSTEM, so our /W4 /WX and
# -Werror never see its warnings. Upstream's own build files are not
# vendored: these targets replace them.

if(MSVC)
    set(YSP_TP_NOWARN /W0)
else()
    set(YSP_TP_NOWARN -w)
endif()

# HarfBuzz from its amalgamation. C++ without exceptions and RTTI, as the
# layout probe built it.
add_library(ysp_harfbuzz STATIC harfbuzz/src/harfbuzz.cc)
target_include_directories(ysp_harfbuzz SYSTEM PUBLIC harfbuzz/src)
target_compile_options(ysp_harfbuzz PRIVATE ${YSP_TP_NOWARN})
set_target_properties(ysp_harfbuzz PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(ysp_harfbuzz PRIVATE /bigobj /GR- /EHs-c-)
    target_compile_definitions(ysp_harfbuzz PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ysp_harfbuzz PRIVATE -fno-exceptions -fno-rtti)
    if(MINGW)
        target_compile_options(ysp_harfbuzz PRIVATE -Wa,-mbig-obj)
    endif()
endif()

# SheenBidi as one translation unit (SheenBidi.c includes the others).
add_library(ysp_sheenbidi STATIC sheenbidi/Source/SheenBidi.c)
target_compile_definitions(ysp_sheenbidi PUBLIC SB_CONFIG_UNITY)
target_include_directories(ysp_sheenbidi SYSTEM PUBLIC sheenbidi/Headers)
target_include_directories(ysp_sheenbidi PRIVATE sheenbidi/Source)
target_compile_options(ysp_sheenbidi PRIVATE ${YSP_TP_NOWARN})

# libunibreak: the files its own Makefile.gcc compiles (the *data.c files are
# included by them).
add_library(ysp_unibreak STATIC
    libunibreak/src/linebreak.c libunibreak/src/linebreakdata.c libunibreak/src/linebreakdef.c
    libunibreak/src/wordbreak.c libunibreak/src/graphemebreak.c libunibreak/src/eastasianwidthdef.c
    libunibreak/src/emojidef.c libunibreak/src/unibreakbase.c libunibreak/src/unibreakdef.c)
target_include_directories(ysp_unibreak SYSTEM PUBLIC libunibreak/src)
target_compile_options(ysp_unibreak PRIVATE ${YSP_TP_NOWARN})

add_library(ysp_budoux STATIC budouxc/src/budoux.c)
target_include_directories(ysp_budoux SYSTEM PUBLIC budouxc/include)
target_compile_options(ysp_budoux PRIVATE ${YSP_TP_NOWARN})

# Skribidi's layout and editor. emoji_presentation_scanner.c is included by
# skb_common.c, so it is not compiled on its own. C17 with extensions, as
# upstream sets it.
add_library(ysp_skribidi STATIC
    skribidi/src/skb_attribute_collection.c skribidi/src/skb_attributes.c skribidi/src/skb_common.c
    skribidi/src/skb_editor.c skribidi/src/skb_editor_rules.c skribidi/src/skb_font_collection.c
    skribidi/src/skb_icon_collection.c skribidi/src/skb_layout.c skribidi/src/skb_rich_layout.c
    skribidi/src/skb_rich_text.c skribidi/src/skb_text.c)
target_include_directories(ysp_skribidi SYSTEM PUBLIC skribidi/include)
target_include_directories(ysp_skribidi PRIVATE skribidi/src)
target_compile_options(ysp_skribidi PRIVATE ${YSP_TP_NOWARN})
set_target_properties(ysp_skribidi PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_link_libraries(ysp_skribidi PUBLIC ysp_harfbuzz PRIVATE ysp_sheenbidi ysp_unibreak ysp_budoux)
if(NOT WIN32)
    target_link_libraries(ysp_skribidi PUBLIC m)
endif()

# The patch hashes the layout glue logs (ylay_get_versions, the pack
# manifest), in the order the patches apply; the pins themselves are in
# pack/layout.c. The patches' hashes say what should be applied; the
# patched file's hash against the manifest says that it is. A tree that
# differs still builds (to compare with upstream, for example), but its
# logged versions say so.
set(patches "")
foreach(name skribidi-0001-bidi-run-merge skribidi-0002-l1-trailing-whitespace skribidi-0003-font-fallback skribidi-0004-emoji-family-fallback
        skribidi-0005-text-grapheme-breaks skribidi-0006-caret-split-grapheme skribidi-0007-monotone-clusters
        skribidi-0008-editor-empty-text skribidi-0009-line-end-offset)
    file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/patches/${name}.patch sha)
    list(APPEND patches "sha256:${sha}")
endforeach()
string(JOIN "," patches ${patches})
# Each source file a patch changes.
set(tree_differs OFF)
foreach(src skb_layout skb_font_collection skb_text skb_editor skb_rich_layout)
    file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/skribidi/src/${src}.c src_sha)
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/MANIFEST.sha256 src_line REGEX " skribidi/src/${src}\\.c$")
    string(REGEX MATCH "^[0-9a-f]+" src_want "${src_line}")
    if(NOT src_sha STREQUAL src_want)
        message(WARNING "third_party/skribidi/src/${src}.c differs from MANIFEST.sha256: the logged versions say tree-differs")
        set(tree_differs ON)
    endif()
endforeach()
if(tree_differs)
    set(patches "${patches},tree-differs")
endif()
set(YSP_SKRIBIDI_PATCHES ${patches} PARENT_SCOPE)
