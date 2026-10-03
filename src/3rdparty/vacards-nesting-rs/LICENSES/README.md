# Third-party license inventory

`jagua-rs` 0.8.0 at commit
`9a19409bd38f3643c3d6d2d7571cddfba548ee17` is licensed under MPL-2.0.
Its source is retained in `vendor/jagua-rs/`; the exact upstream license is
stored beside this inventory as `jagua-rs-MPL-2.0.txt`.

The fixed-container optimizer's bounded solution archive and transactional
ruin/recreate policy are original VACards code informed by the architecture of
Jeroen Gardeyn's Sparrow project. No Sparrow CLI, TUI, signal handling,
strip-packing model, or file-output code is included. Sparrow is MIT-licensed;
its notice is stored beside this inventory as `sparrow-MIT.txt`.

Each transitive Cargo package remains under its own declared license. Cargo's
vendored package metadata and included license files are the authoritative
inventory for Phase 0; release packaging will generate a consolidated notice
from the committed lockfile before nesting is enabled by default.
