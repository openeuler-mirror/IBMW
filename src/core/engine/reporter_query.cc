#include "extension_api/ibmw_reporter_v1.h"

#ifdef IBMW_ENABLE_REPORTER_V1
extern "C" const ibmw_reporter_v1* ibmw_reporter_query_enabled_v1(void);
extern "C" const ibmw_reporter_batch_v1*
ibmw_reporter_query_batch_enabled_v1(void);
#endif

extern "C" IBMW_REPORTER_EXPORT const ibmw_reporter_v1*
ibmw_reporter_query_v1(void) {
#ifdef IBMW_ENABLE_REPORTER_V1
  return ibmw_reporter_query_enabled_v1();
#else
  return nullptr;
#endif
}

extern "C" IBMW_REPORTER_EXPORT const ibmw_reporter_batch_v1*
ibmw_reporter_query_batch_v1(void) {
#ifdef IBMW_ENABLE_REPORTER_V1
  return ibmw_reporter_query_batch_enabled_v1();
#else
  return nullptr;
#endif
}
