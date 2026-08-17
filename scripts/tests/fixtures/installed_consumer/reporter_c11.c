#include <extension_api/ibmw_reporter_v1.h>

#include <stddef.h>

_Static_assert(IBMW_REPORTER_ABI_VERSION_V1 == 1, "reporter version changed");
_Static_assert(sizeof(ibmw_reporter_v1) == 104, "reporter size changed");
_Static_assert(offsetof(ibmw_reporter_v1, owner) == 8, "owner offset changed");
_Static_assert(offsetof(ibmw_reporter_v1, add_signed) == 96, "add_signed offset changed");

int main(void) { return 0; }
