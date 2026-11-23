#include "parcel/serialize/math_io.hpp"

namespace parcel::serialize {

Result<void> WriteVec2(BinaryWriter& writer, const Vec2& value) {
    PARCEL_TRY(writer.WriteF32(value.x));
    PARCEL_TRY(writer.WriteF32(value.y));
    return {};
}

Result<void> WriteVec3(BinaryWriter& writer, const Vec3& value) {
    PARCEL_TRY(writer.WriteF32(value.x));
    PARCEL_TRY(writer.WriteF32(value.y));
    PARCEL_TRY(writer.WriteF32(value.z));
    return {};
}

Result<void> WriteVec4(BinaryWriter& writer, const Vec4& value) {
    PARCEL_TRY(writer.WriteF32(value.x));
    PARCEL_TRY(writer.WriteF32(value.y));
    PARCEL_TRY(writer.WriteF32(value.z));
    PARCEL_TRY(writer.WriteF32(value.w));
    return {};
}

Result<void> WriteQuat(BinaryWriter& writer, const Quat& value) {
    PARCEL_TRY(writer.WriteF32(value.x));
    PARCEL_TRY(writer.WriteF32(value.y));
    PARCEL_TRY(writer.WriteF32(value.z));
    PARCEL_TRY(writer.WriteF32(value.w));
    return {};
}

Result<void> WriteBounds(BinaryWriter& writer, const Bounds& value) {
    PARCEL_TRY(WriteVec3(writer, value.center));
    PARCEL_TRY(WriteVec3(writer, value.extents));
    return {};
}

Result<void> WriteMat4(BinaryWriter& writer, const Mat4& value) {
    for (float component : value.m) {
        PARCEL_TRY(writer.WriteF32(component));
    }
    return {};
}

Result<Vec2> ReadVec2(BinaryReader& reader) {
    Vec2 value;
    PARCEL_TRY_ASSIGN(value.x, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.y, reader.ReadF32());
    return value;
}

Result<Vec3> ReadVec3(BinaryReader& reader) {
    Vec3 value;
    PARCEL_TRY_ASSIGN(value.x, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.y, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.z, reader.ReadF32());
    return value;
}

Result<Vec4> ReadVec4(BinaryReader& reader) {
    Vec4 value;
    PARCEL_TRY_ASSIGN(value.x, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.y, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.z, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.w, reader.ReadF32());
    return value;
}

Result<Quat> ReadQuat(BinaryReader& reader) {
    Quat value;
    PARCEL_TRY_ASSIGN(value.x, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.y, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.z, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.w, reader.ReadF32());
    return value;
}

Result<Bounds> ReadBounds(BinaryReader& reader) {
    Bounds value;
    PARCEL_TRY_ASSIGN(value.center, ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.extents, ReadVec3(reader));
    return value;
}

Result<Mat4> ReadMat4(BinaryReader& reader) {
    Mat4 value;
    for (float& component : value.m) {
        PARCEL_TRY_ASSIGN(component, reader.ReadF32());
    }
    return value;
}

} // namespace parcel::serialize
