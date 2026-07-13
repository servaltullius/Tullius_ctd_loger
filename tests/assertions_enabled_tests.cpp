#ifdef NDEBUG
#error "NDEBUG must not be defined for SkyrimDiag test executables"
#endif

#include <cassert>

int main()
{
  bool evaluated = false;
  assert((evaluated = true));
  return evaluated ? 0 : 1;
}
