# lsl: internal-abi
# Reprezentace hodnot na veřejné dynamické hranici je společná všem backendům.
# Jazykové Int zůstává signed 64-bit i při změně velikosti cílového pointeru.

INT_BITS = 64
TAG_STRING = 1
TAG_LIST = 2
TAG_DICTIONARY = 3
TAG_BOOL = 4
TAG_NONE = 5
TAG_FLOAT = 6
TAG_TUPLE = 7
TAG_REAL_ARRAY = 8
TAG_INT = 9
TAG_FUNCTION = 10
TAG_NATIVE_LIBRARY = 11
TAG_NATIVE_FUNCTION = 12
TAG_INT_ARRAY = 13
TAG_FLOAT_ARRAY = 14
TAG_BYTE_ARRAY = 15
TAG_SET = 16

# Raw typed Int není heap objekt; veřejné Any používá TAG_INT.
function scalar_tag(value_type):
    if value_type == "Int":
        return 0
    elif value_type == "Bool":
        return TAG_BOOL
    elif value_type == "None":
        return TAG_NONE
    elif value_type == "Float":
        return TAG_FLOAT
    elif value_type == "Function":
        return TAG_FUNCTION
    end
    return -1
end
