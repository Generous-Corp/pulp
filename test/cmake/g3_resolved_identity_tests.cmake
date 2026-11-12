# Bounded G3 D5 resolved identity public consumer. The producer and receipt
# vectors are covered by the host test; this target proves installed headers.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../examples/g3-resolved-identity-consumer"
                 "${CMAKE_BINARY_DIR}/examples/g3-resolved-identity-consumer")
