export module vecgltf.spirv;

export namespace vecgltf::shaders
{
template<decltype(sizeof(0)) N>
struct Code {
    unsigned words[N];
};
template<decltype(sizeof(0)) N>
consteval auto decode(const unsigned char (&bytes)[N]) {
    static_assert(sizeof(unsigned) == 4 && N >= 20 && N % 4 == 0);
    Code<N / 4> result {};
    for (decltype(N) i = 0; i < N / 4; ++i)
        result.words[i] = unsigned(bytes[i * 4]) | (unsigned(bytes[i * 4 + 1]) << 8) |
                          (unsigned(bytes[i * 4 + 2]) << 16) | (unsigned(bytes[i * 4 + 3]) << 24);
    return result;
}
} // namespace vecgltf::shaders
