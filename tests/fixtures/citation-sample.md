## §7 LayerResolver.cpp -- `DarkShadow`

The layer table `@0x27c198c20` names it `DesignLibrary`, decoded as
`GlassMaterialProvider.Configuration` and cross-checked against
`GlassEdgeMaterialProvider.resolveLayers`. See `WindowControlColors.h` for
the unrelated file this must NOT be read as a symbol. Also cites 0x27c198c20
again, bare, and a mangled name `_$s7SwiftUI19ColorSchemeContrastO`.

A second mention of `GlassMaterialProvider.Configuration` proves it dedupes
to one Symbol. `Assets/Icons.Bundle` is a path, not a symbol. A bare token
0x1a2b looks hex but is too short to be an address.

A struct descriptor reads flags=0x00000052 and a float's bit pattern is
0x00000000 -- six-plus hex digits each, same as the real address above, but
neither is cache-shaped (both start with zero) and neither may become an
Address citation. `SwiftUI.framework`, `libswiftCore.dylib` and `Field.frag`
are binary/asset file names, not symbols, same as `WindowControlColors.h`
above. Neither is `Foo.MD` a symbol -- the extension check folds case on the
token's own letters, not only on the known extension's.
