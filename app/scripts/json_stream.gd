extends RefCounted

# Write one message as continuation frames without making a String for the
# whole scene. Godot String/JSON lengths are signed 32-bit, even though the
# renderer protocol and PackedByteArray support larger aggregate transfers.
# Packed arrays become {"$buffer": index, "type": name} in the JSON chain; the
# top-level "buffers" lists their byte lengths and a second chain carries them.
const MORE_FRAMES := 0x80000000
var _pipe: FileAccess
var _limit := 64 * 1024 * 1024
var _buffer := PackedByteArray()
var _canceled: Callable
var _failed := false
var _buffers: Array = []

func write(pipe: FileAccess, message: Dictionary, frame_limit: int, canceled := Callable()) -> bool:
	_pipe = pipe; _limit = frame_limit; _canceled = canceled
	_buffer = PackedByteArray(); _failed = false; _buffers = []
	if _limit < 1 or _limit > 64 * 1024 * 1024:
		return false
	_value(message, true)
	if not _failed: _flush(false)
	var total := 0
	for data: PackedByteArray in _buffers: total += data.size()
	for data: PackedByteArray in _buffers:
		if _failed: break
		total -= data.size()
		_append_bytes(data, total > 0)
	if not _failed and not _buffer.is_empty(): _flush(false)
	_buffer = PackedByteArray(); _buffers = []
	_pipe = null; _canceled = Callable()
	return not _failed

func _flush(more: bool) -> void:
	if _failed: return
	if _canceled.is_valid() and _canceled.call():
		_failed = true
		return
	var flags := _buffer.size() | (MORE_FRAMES if more else 0)
	if not _pipe.store_32(flags) or not _pipe.store_buffer(_buffer): _failed = true
	_buffer = PackedByteArray()

# Flush only when another byte follows, so the last frame is nonempty even
# when the chain length is an exact multiple of the limit.
func _append_bytes(bytes: PackedByteArray, more_follows := true) -> void:
	var offset := 0
	while offset < bytes.size() and not _failed:
		if _buffer.size() == _limit: _flush(true)
		if _failed: return
		var count := mini(_limit - _buffer.size(), bytes.size() - offset)
		_buffer.append_array(bytes if offset == 0 and count == bytes.size() else bytes.slice(offset, offset + count))
		offset += count
	if not more_follows and not _failed and not _buffer.is_empty(): _flush(false)

func _append(text: String) -> void:
	if _failed: return
	_append_bytes(text.to_utf8_buffer())

func _packed(value: Variant) -> String:
	if value is PackedVector3Array: return "vec3"
	if value is PackedVector2Array: return "vec2"
	if value is PackedInt32Array: return "i32"
	if value is PackedFloat32Array: return "f32"
	if value is PackedByteArray: return "bytes"
	return ""

func _value(value: Variant, top := false) -> void:
	if _failed: return
	var type := _packed(value)
	if not type.is_empty():
		_append('{"$buffer":%d,"type":"%s"}' % [_buffers.size(), type])
		_buffers.append(value if value is PackedByteArray else value.to_byte_array())
	elif value is Dictionary:
		_append("{")
		var first := true
		for key in value:
			if _failed: return
			if not first: _append(",")
			first = false
			_append(JSON.stringify(String(key)) + ":")
			_value(value[key])
		if top and not _buffers.is_empty():
			_append(("," if not first else "") + '"buffers":' + JSON.stringify(_buffers.map(func(data: PackedByteArray): return data.size())))
		_append("}")
	elif value is Array:
		# Short scalar vectors/matrices can use the built-in formatter directly.
		if value.size() <= 64 and value.all(func(item: Variant): return not item is Array and not item is Dictionary and _packed(item).is_empty()):
			_append(JSON.stringify(value))
			return
		_append("[")
		for i in value.size():
			if _failed: return
			if i: _append(",")
			_value(value[i])
		_append("]")
	else:
		_append(JSON.stringify(value))
