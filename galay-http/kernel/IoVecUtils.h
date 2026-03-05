#ifndef GALAY_IOVEC_UTILS_H
#define GALAY_IOVEC_UTILS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/uio.h>
#include <vector>

namespace galay::kernel {

class IoVecWindow {
public:
    static size_t buildWindow(const struct iovec* source,
                              size_t source_count,
                              std::vector<struct iovec>& out) {
        out.clear();
        if (source == nullptr || source_count == 0) {
            return 0;
        }

        out.reserve(source_count);
        for (size_t i = 0; i < source_count; ++i) {
            if (source[i].iov_len == 0) {
                continue;
            }
            out.push_back(source[i]);
        }
        return out.size();
    }

    static size_t buildWindow(const std::vector<struct iovec>& source,
                              std::vector<struct iovec>& out) {
        return buildWindow(source.data(), source.size(), out);
    }

    template<size_t N>
    static size_t buildWindow(const std::array<struct iovec, N>& source,
                              size_t source_count,
                              std::vector<struct iovec>& out) {
        return buildWindow(source.data(), std::min(source_count, N), out);
    }

    static const struct iovec* firstNonEmpty(const struct iovec* source,
                                             size_t source_count) noexcept {
        if (source == nullptr || source_count == 0) {
            return nullptr;
        }
        for (size_t i = 0; i < source_count; ++i) {
            if (source[i].iov_len > 0) {
                return &source[i];
            }
        }
        return nullptr;
    }

    static const struct iovec* firstNonEmpty(const std::vector<struct iovec>& source) noexcept {
        return firstNonEmpty(source.data(), source.size());
    }

    template<size_t N>
    static const struct iovec* firstNonEmpty(const std::array<struct iovec, N>& source,
                                             size_t source_count) noexcept {
        return firstNonEmpty(source.data(), std::min(source_count, N));
    }

    static bool bindFirstNonEmpty(const struct iovec* source,
                                  size_t source_count,
                                  char*& buffer,
                                  size_t& length) noexcept {
        const struct iovec* first = firstNonEmpty(source, source_count);
        if (first == nullptr) {
            buffer = nullptr;
            length = 0;
            return false;
        }

        buffer = static_cast<char*>(first->iov_base);
        length = first->iov_len;
        return length > 0;
    }

    static bool bindFirstNonEmpty(const std::vector<struct iovec>& source,
                                  char*& buffer,
                                  size_t& length) noexcept {
        return bindFirstNonEmpty(source.data(), source.size(), buffer, length);
    }

    template<size_t N>
    static bool bindFirstNonEmpty(const std::array<struct iovec, N>& source,
                                  size_t source_count,
                                  char*& buffer,
                                  size_t& length) noexcept {
        return bindFirstNonEmpty(source.data(), std::min(source_count, N), buffer, length);
    }
};

class IoVecBytes {
public:
    static size_t sum(const struct iovec* source, size_t source_count) noexcept {
        if (source == nullptr || source_count == 0) {
            return 0;
        }

        size_t total = 0;
        for (size_t i = 0; i < source_count; ++i) {
            total += source[i].iov_len;
        }
        return total;
    }

    template<size_t N>
    static size_t sum(const std::array<struct iovec, N>& source, size_t source_count) noexcept {
        return sum(source.data(), std::min(source_count, N));
    }

    static size_t sum(const std::vector<struct iovec>& source) noexcept {
        return sum(source.data(), source.size());
    }

    static size_t copyPrefix(const struct iovec* source,
                             size_t source_count,
                             uint8_t* destination,
                             size_t requested) noexcept {
        if (source == nullptr || source_count == 0 || destination == nullptr || requested == 0) {
            return 0;
        }

        size_t copied = 0;
        for (size_t i = 0; i < source_count && copied < requested; ++i) {
            const auto* src = static_cast<const uint8_t*>(source[i].iov_base);
            if (src == nullptr || source[i].iov_len == 0) {
                continue;
            }

            const size_t bytes = std::min(source[i].iov_len, requested - copied);
            std::memcpy(destination + copied, src, bytes);
            copied += bytes;
        }
        return copied;
    }

    template<size_t N>
    static size_t copyPrefix(const std::array<struct iovec, N>& source,
                             size_t source_count,
                             uint8_t* destination,
                             size_t requested) noexcept {
        return copyPrefix(source.data(), std::min(source_count, N), destination, requested);
    }

    static size_t copyPrefix(const std::vector<struct iovec>& source,
                             uint8_t* destination,
                             size_t requested) noexcept {
        return copyPrefix(source.data(), source.size(), destination, requested);
    }
};

class IoVecCursor {
public:
    IoVecCursor() = default;

    explicit IoVecCursor(const std::vector<struct iovec>& iovecs) {
        reset(iovecs);
    }

    explicit IoVecCursor(std::vector<struct iovec>&& iovecs) {
        reset(std::move(iovecs));
    }

    void reset(const std::vector<struct iovec>& iovecs) {
        m_iovecs = iovecs;
        normalize();
    }

    void reset(std::vector<struct iovec>&& iovecs) {
        m_iovecs = std::move(iovecs);
        normalize();
    }

    [[nodiscard]] const struct iovec* data() const noexcept {
        if (empty()) {
            return nullptr;
        }
        return m_iovecs.data() + m_index;
    }

    [[nodiscard]] struct iovec* data() noexcept {
        if (empty()) {
            return nullptr;
        }
        return m_iovecs.data() + m_index;
    }

    [[nodiscard]] size_t count() const noexcept {
        return m_index >= m_iovecs.size() ? 0 : (m_iovecs.size() - m_index);
    }

    [[nodiscard]] bool empty() const noexcept {
        return count() == 0;
    }

    [[nodiscard]] size_t remainingBytes() const noexcept {
        return m_remaining_bytes;
    }

    size_t advance(size_t bytes) noexcept {
        if (bytes == 0 || empty()) {
            return 0;
        }

        size_t to_advance = std::min(bytes, m_remaining_bytes);
        size_t remaining = to_advance;

        while (remaining > 0 && m_index < m_iovecs.size()) {
            auto& seg = m_iovecs[m_index];
            if (remaining < seg.iov_len) {
                seg.iov_base = static_cast<char*>(seg.iov_base) + remaining;
                seg.iov_len -= remaining;
                remaining = 0;
                break;
            }

            remaining -= seg.iov_len;
            seg.iov_len = 0;
            ++m_index;
        }

        const size_t advanced = to_advance - remaining;
        m_remaining_bytes -= advanced;
        skipLeadingEmpty();

        if (m_index >= m_iovecs.size()) {
            m_iovecs.clear();
            m_index = 0;
        }

        return advanced;
    }

    void exportWindow(std::vector<struct iovec>& out) const {
        out.clear();
        if (empty()) {
            return;
        }
        out.insert(out.end(), m_iovecs.begin() + static_cast<std::ptrdiff_t>(m_index), m_iovecs.end());
    }

private:
    void normalize() {
        size_t write_index = 0;
        m_remaining_bytes = 0;

        for (size_t read_index = 0; read_index < m_iovecs.size(); ++read_index) {
            const auto& seg = m_iovecs[read_index];
            if (seg.iov_len == 0) {
                continue;
            }
            if (write_index != read_index) {
                m_iovecs[write_index] = seg;
            }
            m_remaining_bytes += seg.iov_len;
            ++write_index;
        }

        m_iovecs.resize(write_index);
        m_index = 0;
    }

    void skipLeadingEmpty() {
        while (m_index < m_iovecs.size() && m_iovecs[m_index].iov_len == 0) {
            ++m_index;
        }
    }

    std::vector<struct iovec> m_iovecs;
    size_t m_index = 0;
    size_t m_remaining_bytes = 0;
};

} // namespace galay::kernel

#endif // GALAY_IOVEC_UTILS_H
