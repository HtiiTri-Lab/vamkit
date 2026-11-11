// The producer may run concurrently and visits slots in arbitrary order. InterlockedMax
// of these dimensions gives a legal 2D dispatch; the consumer discards its final padding.
#define HS_ID_GROUPS_PER_ROW 65535u
uint2 HsLiveGroups(uint slot) {
    uint group = slot >> 6;
    return uint2(min(group + 1u, HS_ID_GROUPS_PER_ROW), group / HS_ID_GROUPS_PER_ROW + 1u);
}
uint HsLiveIndex(uint3 tid) { return tid.x + tid.y * (HS_ID_GROUPS_PER_ROW * 64u); }
// The group list: one thread group per slot.
uint2 HsGroupGroups(uint slot) { return uint2(min(slot + 1u, HS_ID_GROUPS_PER_ROW), slot / HS_ID_GROUPS_PER_ROW + 1u); }
uint HsGroupIndex(uint3 group) { return group.x + group.y * HS_ID_GROUPS_PER_ROW; }
