/* @LICENSE(MUSLC_MIT) */

#include <string.h>

char *strtok_r(char *s, const char *sep, char **p)
{
	if (!s && !(s = *p)) return NULL;
	s += strspn(s, sep);
	if (!*s) return *p = 0;
	*p = s + strcspn(s, sep);
	if (**p) *(*p)++ = 0;
	else *p = 0;
	return s;
}
