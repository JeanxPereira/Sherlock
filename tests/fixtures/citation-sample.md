## §7 LayerResolver.cpp -- `DarkShadow`

The layer table `@0x27c198c20` names it `DesignLibrary`, decoded as
`GlassMaterialProvider.Configuration` and cross-checked against
`GlassEdgeMaterialProvider.resolveLayers`. See `WindowControlColors.h` for
the unrelated file this must NOT be read as a symbol. Also cites 0x27c198c20
again, bare, and a mangled name `_$s7SwiftUI19ColorSchemeContrastO`.

A second mention of `GlassMaterialProvider.Configuration` proves it dedupes
to one Symbol. `Assets/Icons.Bundle` is a path, not a symbol. A bare token
0x1a2b looks hex but is too short to be an address.
