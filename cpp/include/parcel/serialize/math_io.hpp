#pragma once

#include "parcel/core/result.hpp"
#include "parcel/math.hpp"
#include "parcel/serialize/binary.hpp"

namespace parcel::serialize {

[[nodiscard]] Result<void> WriteVec2(BinaryWriter& writer, const Vec2& value);
[[nodiscard]] Result<void> WriteVec3(BinaryWriter& writer, const Vec3& value);
[[nodiscard]] Result<void> WriteVec4(BinaryWriter& writer, const Vec4& value);
[[nodiscard]] Result<void> WriteQuat(BinaryWriter& writer, const Quat& value);
[[nodiscard]] Result<void> WriteBounds(BinaryWriter& writer, const Bounds& value);
[[nodiscard]] Result<void> WriteMat4(BinaryWriter& writer, const Mat4& value);

[[nodiscard]] Result<Vec2> ReadVec2(BinaryReader& reader);
[[nodiscard]] Result<Vec3> ReadVec3(BinaryReader& reader);
[[nodiscard]] Result<Vec4> ReadVec4(BinaryReader& reader);
[[nodiscard]] Result<Quat> ReadQuat(BinaryReader& reader);
[[nodiscard]] Result<Bounds> ReadBounds(BinaryReader& reader);
[[nodiscard]] Result<Mat4> ReadMat4(BinaryReader& reader);

} // namespace parcel::serialize
