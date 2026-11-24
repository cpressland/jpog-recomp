/* jpog, the program: the host's jpog_main (main.c). The app will load the
 * same game as libjpog.dylib and enter it the same way. */
int jpog_main(int argc, char **argv);

int main(int argc, char **argv) { return jpog_main(argc, argv); }
