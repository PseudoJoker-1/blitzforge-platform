# Lua 5.4.7, vendored

Source:  https://www.lua.org/ftp/lua-5.4.7.tar.gz
sha256:  9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
Date:    2026-08-10
License: MIT (see lua/README in the archive; Lua is MIT since 5.0)

Only `src/` is vendored, minus `lua.c` and `luac.c` - those are the standalone
interpreter and compiler, each with its own `main()`, and we embed the library.

Nothing here is modified. If it ever needs patching, add the patch as a file
beside this one and say why, so the next update can reapply it deliberately.
