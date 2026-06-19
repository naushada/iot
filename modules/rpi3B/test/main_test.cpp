#ifndef __main_test_cpp__
#define __main_test_cpp__

#include "gpio_test.hpp"


int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}



#endif /*__main_test_cpp__*/