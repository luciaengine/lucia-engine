export function readRoomInvite(fragment) {
  const fields = new URLSearchParams(fragment.replace(/^#/, ''));
  const room = fields.get('room') ?? '';
  const token = fields.get('token') ?? '';
  if (fields.getAll('room').length !== 1 || fields.getAll('token').length !== 1 ||
      !/^[a-zA-Z0-9_-]{1,80}$/.test(room) || token.length < 24 || token.length > 512) {
    return null;
  }
  return { room, token };
}
