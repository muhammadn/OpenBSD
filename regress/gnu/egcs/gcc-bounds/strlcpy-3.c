#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
	char buf[10];
	strlcpy(buf, "foo", 15);
	return 1;
}
