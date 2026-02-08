enable_testing()

set(FETCHCONTENT_BASE_DIR ${PROJECT_SOURCE_DIR}/.dependencies)

find_package(GTest)

file(GLOB thread_pool_test_SOURCES CONFIGURE_DEPENDS "*.cpp")

# 2. 테스트 실행 파일 생성
add_executable(thread_pool_test ${thread_pool_test_SOURCES})
# 3. 라이브러리 링크
# gtest_main: 구글 테스트가 제공하는 기본 main() 함수 포함 (별도 main 작성 불필요)
target_link_libraries(thread_pool_test PRIVATE
        GTest::gtest
        GTest::gtest_main
        thread_pool_lib
)

# 4. CTest에 테스트 등록 (자동 검색)
include(GoogleTest)
gtest_discover_tests(thread_pool_test)