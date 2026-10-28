# Writable stub metadata and executable entries

Horizon's CodeMemory provides simultaneous RW and RX views. The native HotSpot
port keeps `CodeBlob` and auxiliary stub headers in writable memory, and exposes
instruction addresses through RX. A code buffer's instruction-store helper
alone cannot establish that contract: the interpreter and inline-cache queues
also contain ordinary mutable C++ objects inside the code allocation.

The changes in `share/code/stubs.hpp`, `stubs.cpp`, `icBuffer.hpp` and
`share/interpreter/interpreter.hpp` enforce these invariants:

- `StubQueue::_stub_buffer`, `stub_at`, `first`, `next` and the objects passed to
  `initialize`/`finalize` remain RW. Their queue indices and sizes are unchanged.
- `InterpreterCodelet::code_begin/end`, `ICStub::code_begin/end` and public
  `StubQueue::code_start/end` return RX bounds. Queue containment tests compare
  actual executable PCs against those bounds.
- `compute_stub_size` maps the public code start back to RW before alignment and
  subtraction from the RW header. It never narrows an unrelated RX–RW delta.
- `ICStub_from_destination_address` maps an executable destination back to RW
  before subtracting the aligned header size. Returned headers are writable.

Mapping the header address before adding a code-end size avoids relying on
ambiguous one-past-arena address classification. RW/RX aliases are page aligned,
so the existing intra-page code-entry alignment is preserved. All translation
helpers are identity operations outside native Horizon; Zero behavior is kept.

The following runtime paths exercise the design:

1. Native template-interpreter initialization requests codelets, writes their
   descriptive headers, emits real instructions through CodeBuffer, commits
   their sizes, and trims the unused queue tail.
2. The inline-cache buffer allocates transition stubs, writes `_ic_site`, exposes
   the executable trampoline, then recovers and clears its RW header later.
3. Runtime interpreter membership/stack walking looks up RX PCs while returning
   ordinary RW codelet descriptors.

Native compilation and actual JVM/device execution remain necessary validation.
The standalone exception ABI probe does not validate these code-cache structures.

The companion audit reported additional sites to their owners: nmethod header
offsets and deoptimization entries, CodeBlob trimming, VtableStub headers,
constant/inline-metadata relocation stores, post-call-nop encoder/decoder pairs,
and AArch64 literal reads from oop tables. Those changes are outside this
four-file patch and must be reviewed together with the complete backend.
