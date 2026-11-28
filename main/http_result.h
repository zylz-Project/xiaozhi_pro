#ifndef HTTP_RESULT_H_
#define HTTP_RESULT_H_

inline int HttpIntValue(int result) { return result; }

template <typename Result>
int HttpIntValue(const Result& result) {
    return result.value_or(-1);
}

#endif
