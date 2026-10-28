extends SceneTree

const LayoutGeometry = preload("res://scripts/layout_geometry.gd")
var failures := 0

func check(condition: bool, message: String) -> void:
	if not condition:
		push_error(message)
		failures += 1

func mesh_data(path: String, transform: Transform3D) -> Dictionary:
	return {"id": path, "source_path": "/prototype/mesh", "prototype": 0, "transform": [
		transform.basis.x.x, transform.basis.x.y, transform.basis.x.z, 0,
		transform.basis.y.x, transform.basis.y.y, transform.basis.y.z, 0,
		transform.basis.z.x, transform.basis.z.y, transform.basis.z.z, 0,
		transform.origin.x, transform.origin.y, transform.origin.z, 1]}

func _initialize() -> void:
	var geometry := LayoutGeometry.new()
	var prototypes := [{"surfaces": [{"positions": PackedVector3Array([Vector3(-1,-1,0), Vector3(1,-1,0), Vector3(0,1,0)]), "indices": PackedInt32Array([0,1,2])}]}]
	var near_transform := Transform3D(Basis.from_scale(Vector3(-2, 0.5, 1)), Vector3(0, 0, 2))
	var data := {"prototypes": prototypes, "meshes": [mesh_data("/far", Transform3D.IDENTITY), mesh_data("/near", near_transform)]}
	geometry.rebuild(data)
	check(geometry.pick(Vector3(0,0,5), Vector3.FORWARD) == "/near", "Picking must find the nearest transformed triangle, regardless of instance order")
	var hit: Dictionary = geometry.raycast(Vector3(0,0,5), Vector3.FORWARD)
	check(hit.position.is_equal_approx(Vector3(0,0,2)) and hit.normal.is_equal_approx(Vector3.BACK), "Depth picking must return world position and a facing normal under mirrored scaling")
	hit = geometry.raycast(Vector3(0,0,5), Vector3.FORWARD, "/near")
	check(hit.path == "/far" and hit.position.is_zero_approx(), "Placement must ignore the moved object's own geometry")
	check(geometry.raycast(Vector3(0,0,5), Vector3.FORWARD, "/prototype").is_empty(), "Shared captured geometry must be excludable by authored path")
	check(geometry.pick(Vector3(100,0,5), Vector3.FORWARD).is_empty(), "A ray outside the bounds must miss")
	check(geometry.entries[0].geometry == geometry.entries[1].geometry, "Repeated instances must share GPU geometry and picking BVH")
	check(geometry.entries[1].bounds == near_transform * geometry.entries[1].geometry.mesh.get_aabb(), "Framing bounds must include the instance transform")
	check(geometry.entries[1].matches("/prototype"), "Selecting a shared prototype must highlight its instances")
	data.meshes = [mesh_data("/small", Transform3D(Basis.from_scale(Vector3.ONE * 0.001), Vector3.ZERO))]
	geometry.rebuild(data)
	check(geometry.pick(Vector3(0,0,1), Vector3.FORWARD) == "/small", "Small unit-converted meshes must remain pickable")
	data.meshes = [mesh_data("/moved", Transform3D(Basis.IDENTITY, Vector3(10,0,0)))]
	geometry.rebuild(data)
	check(geometry.pick(Vector3(0,0,5), Vector3.FORWARD).is_empty(), "An updated snapshot must discard old picking geometry")
	check(geometry.pick(Vector3(10,0,5), Vector3.FORWARD) == "/moved", "An updated snapshot must use the new transform")
	geometry.rebuild({})
	check(geometry.entries.is_empty(), "Closing a scene must clear the geometry cache")
	print("Layout geometry: %d failures" % failures)
	quit(1 if failures else 0)
