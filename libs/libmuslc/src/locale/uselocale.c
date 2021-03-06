/* @LICENSE(MUSLC_MIT) */

#include "locale_impl.h"
#include "pthread_impl.h"

locale_t uselocale(locale_t l)
{
	pthread_t self = pthread_self();
	locale_t old = self->locale;
	if (l) self->locale = l;
	return old;
}
