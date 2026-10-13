# lsl: internal-abi
# Layout stavu současného Linux runtime; není systémovým volacím ABI.

load abi_x86_64

TLS_ERROR = 0
TLS_ERROR_FLAG = 8
TLS_STACK_TOP = 16
# Worker-only TLS slot, stored in thread-table entry+56. The main TLS block
# ends after TLS_STACK_TOP; its following word belongs to GC_WORKLIST.
TLS_WORKER_STATE = 24
MAIN_TLS = 160
STATE_BYTES = 1344
# Bitmapa začátků objektů: ukazatel a kapacita v bajtech ve stavu; počáteční
# velikost (4 MiB pokryje 256 MiB haldy), roste spolu s brk haldou.
OBJECT_BITMAP = 1328
OBJECT_BITMAP_CAPACITY = 1336
OBJECT_BITMAP_INITIAL = 4194304
GLOBAL_ROOT_BEGIN = 16
GLOBAL_ROOT_END = 24
GC_ALLOCATED_BYTES = 144
GC_BUDGET = 152
GC_WORKLIST = 184
GC_HEADER_BYTES = 32
OBJECT_TAG = abi_x86_64.OBJECT_TAG
