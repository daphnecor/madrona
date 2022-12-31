#include <madrona/physics.hpp>
#include <madrona/context.hpp>

namespace madrona {

using namespace base;
using namespace math;

namespace phys {

struct SolverData {
    Contact *contacts;
    std::atomic<CountT> numContacts;
    CountT maxContacts;
    float deltaT;
    float h;
    Vector3 g;
    float gMagnitude;
    float restitutionThreshold;

    inline SolverData(CountT max_contacts_per_step,
                      float delta_t,
                      CountT num_substeps,
                      Vector3 gravity)
        : contacts((Contact *)rawAlloc(sizeof(Contact) * max_contacts_per_step)),
          numContacts(0),
          maxContacts(max_contacts_per_step),
          deltaT(delta_t),
          h(delta_t / (float)num_substeps),
          g(gravity),
          gMagnitude(gravity.length()),
          restitutionThreshold(2.f * gMagnitude * h)
    {}

    inline void addContacts(Span<const Contact> added_contacts)
    {
        int32_t contact_idx = numContacts.fetch_add(added_contacts.size(),
                                                    std::memory_order_relaxed);
        assert(contact_idx < maxContacts);

        for (CountT i = 0; i < added_contacts.size(); i++) {
            contacts[contact_idx + i] = added_contacts[i];
        }
    }
};

struct ObjectData {
    ObjectManager *mgr;
};

inline void updateCollisionAABB(Context &ctx,
                                const Position &pos,
                                const Rotation &rot,
                                const ObjectID &obj_id,
                                const Velocity &vel,
                                CollisionAABB &out_aabb)
{
    // FIXME: this could all be more efficient with a center + width
    // AABB representation
    ObjectManager &obj_mgr = *ctx.getSingleton<ObjectData>().mgr;

    Mat3x3 rot_mat = Mat3x3::fromQuat(rot);
    AABB obj_aabb = obj_mgr.aabbs[obj_id.idx];

    AABB world_aabb;

    // RTCD page 86
#pragma unroll
    for (CountT i = 0; i < 3; i++) {
        world_aabb.pMin[i] = world_aabb.pMax[i] = pos[i];

#pragma unroll
        for (CountT j = 0; j < 3; j++) {
            float e = rot_mat[i][j] * obj_aabb.pMin[j];
            float f = rot_mat[i][j] * obj_aabb.pMax[j];

            if (e < f) {
                world_aabb.pMin[i] += e;
                world_aabb.pMax[i] += f;
            } else {
                world_aabb.pMin[i] += f;
                world_aabb.pMax[i] += e;
            }
        }
    }

    constexpr float expansion_factor = 2.f;
    constexpr float max_accel = 100.f;

    float delta_t = ctx.getSingleton<SolverData>().deltaT;
    float min_pos_change = max_accel * delta_t * delta_t;

    Vector3 linear_velocity = vel.linear;

#pragma unroll
    for (int32_t i = 0; i < 3; i++) {
        float pos_delta = expansion_factor * linear_velocity[i] * delta_t;

        float min_delta = pos_delta - min_pos_change;
        float max_delta = pos_delta + min_pos_change;

        if (min_delta < 0.f) {
            world_aabb.pMin[i] += min_delta;
        }
        if (max_delta > 0.f) {
            world_aabb.pMax[i] += max_delta;
        }
    }

    out_aabb = world_aabb;
}

namespace narrowphase {

enum class NarrowphaseTest : uint32_t {
    SphereSphere = 1,
    HullHull = 2,
    SphereHull = 3,
    PlanePlane = 4,
    SpherePlane = 5,
    HullPlane = 6,
};

inline void runNarrowphase(
    Context &ctx,
    const CandidateCollision &candidate_collision)
{
    ObjectID a_obj = ctx.getUnsafe<ObjectID>(candidate_collision.a);
    ObjectID b_obj = ctx.getUnsafe<ObjectID>(candidate_collision.b);

    SolverData &solver = ctx.getSingleton<SolverData>();
    const ObjectManager &obj_mgr = *ctx.getSingleton<ObjectData>().mgr;

    const CollisionPrimitive *a_prim = &obj_mgr.primitives[a_obj.idx];
    const CollisionPrimitive *b_prim = &obj_mgr.primitives[b_obj.idx];

    uint32_t raw_type_a = static_cast<uint32_t>(a_prim->type);
    uint32_t raw_type_b = static_cast<uint32_t>(b_prim->type);

    Entity a_entity = candidate_collision.a;
    Entity b_entity = candidate_collision.b;

    if (raw_type_a > raw_type_b) {
        std::swap(raw_type_a, raw_type_b);
        std::swap(a_entity, b_entity);
        std::swap(a_prim, b_prim);
    }

    NarrowphaseTest test_type {raw_type_a | raw_type_b};

    Position a_pos = ctx.getUnsafe<Position>(a_entity);
    Position b_pos = ctx.getUnsafe<Position>(b_entity);

    switch (test_type) {
    case NarrowphaseTest::SphereSphere: {
        float a_radius = a_prim->sphere.radius;
        float b_radius = b_prim->sphere.radius;

        Vector3 to_b = b_pos - a_pos;
        float dist = to_b.length();

        if (dist > 0 && dist < a_radius + b_radius) {
            Vector3 mid = to_b / 2.f;

            Vector3 to_b_normal = to_b / dist;
            solver.addContacts({{
                a_entity,
                b_entity,
                { 
                    makeVector4(a_pos + mid, dist / 2.f),
                    {}, {}, {}
                },
                1,
                to_b_normal,
                0.f,
            }});


            Loc loc = ctx.makeTemporary<CollisionEventTemporary>();
            ctx.getUnsafe<CollisionEvent>(loc) = CollisionEvent {
                candidate_collision.a,
                candidate_collision.b,
            };
        }
    } break;
    case NarrowphaseTest::PlanePlane: {
        // Do nothing, planes must be static.
        // Should rework this entire setup so static objects
        // aren't checked against the BVH
    } break;
    case NarrowphaseTest::SpherePlane: {
        auto sphere = a_prim->sphere;
        Rotation b_rot = ctx.getUnsafe<Rotation>(b_entity);

        constexpr Vector3 base_normal = { 0, 0, 1 };
        Vector3 plane_normal = b_rot.rotateVec(base_normal);

        float d = plane_normal.dot(b_pos);
        float t = plane_normal.dot(a_pos) - d;

        if (t < sphere.radius) {
            solver.addContacts({{
                a_entity,
                b_entity,
                {
                    makeVector4(a_pos + plane_normal * sphere.radius, t),
                    {}, {}, {}
                },
                1,
                plane_normal,
                0.f,
            }});
        }
    } break;
    case NarrowphaseTest::HullHull: {
        // Get half edge mesh for hull A and hull B
        const auto &hEdgeA = a_prim->hull.halfEdgeMesh;
        const auto &hEdgeB = a_prim->hull.halfEdgeMesh;

        auto transformVertex = [&ctx] (math::Vector3 v, Entity &e) {
            Scale e_scale = ctx.getUnsafe<Scale>(e);
            Rotation e_rotation = ctx.getUnsafe<Rotation>(e);
            Position e_position = ctx.getUnsafe<Position>(e);

            return e_position + e_rotation.rotateVec((math::Vector3)e_scale * v);
        };

        geometry::CollisionMesh collisionMeshA;
        collisionMeshA.halfEdgeMesh = &hEdgeA;
        collisionMeshA.vertexCount = hEdgeA.getVertexCount();
        collisionMeshA.vertices = (math::Vector3 *)TmpAllocator::get().alloc(sizeof(math::Vector3) * collisionMeshA.vertexCount);
        collisionMeshA.center = a_pos;
        for (int v = 0; v < collisionMeshA.vertexCount; ++v) {
            collisionMeshA.vertices[v] = transformVertex(hEdgeA.vertex(v), a_entity);
        }

        geometry::CollisionMesh collisionMeshB;
        collisionMeshB.halfEdgeMesh = &hEdgeB;
        collisionMeshB.vertexCount = hEdgeB.getVertexCount();
        collisionMeshB.vertices = (math::Vector3 *)TmpAllocator::get().alloc(sizeof(math::Vector3) * collisionMeshB.vertexCount);
        collisionMeshB.center = b_pos;
        for (int v = 0; v < collisionMeshB.vertexCount; ++v) {
            collisionMeshB.vertices[v] = transformVertex(hEdgeB.vertex(v), b_entity);
        }

        Manifold manifold = doSAT(collisionMeshA, collisionMeshB);

        if (manifold.numContactPoints > 0) {
            solver.addContacts({{
                manifold.aIsReference ? a_entity : b_entity,
                manifold.aIsReference ? b_entity : a_entity,
                {
                    manifold.contactPoints[0],
                    manifold.contactPoints[1],
                    manifold.contactPoints[2],
                    manifold.contactPoints[3],
                },
                manifold.numContactPoints,
                manifold.normal,
                0.f,
            }});
        }
    } break;
    case NarrowphaseTest::SphereHull: {
        assert(false);
    } break;
    case NarrowphaseTest::HullPlane: {
        // Get half edge mesh for hull A and hull B
        const auto &hEdgeA = a_prim->hull.halfEdgeMesh;

        auto transformVertex = [&ctx] (math::Vector3 v, Entity &e) {
            Scale e_scale = ctx.getUnsafe<Scale>(e);
            Rotation e_rotation = ctx.getUnsafe<Rotation>(e);
            Position e_position = ctx.getUnsafe<Position>(e);

            return e_position + e_rotation.rotateVec((math::Vector3)e_scale * v);
        };

        geometry::CollisionMesh collisionMeshA;
        collisionMeshA.halfEdgeMesh = &hEdgeA;
        collisionMeshA.vertexCount = hEdgeA.getVertexCount();
        collisionMeshA.vertices = (math::Vector3 *)TmpAllocator::get().alloc(sizeof(math::Vector3) * collisionMeshA.vertexCount);
        collisionMeshA.center = a_pos;
        for (int v = 0; v < collisionMeshA.vertexCount; ++v) {
            collisionMeshA.vertices[v] = transformVertex(hEdgeA.vertex(v), a_entity);
        }

        Rotation b_rot = ctx.getUnsafe<Rotation>(b_entity);
        constexpr Vector3 base_normal = { 0, 0, 1 };
        Vector3 plane_normal = b_rot.rotateVec(base_normal);

        geometry::Plane plane = { b_pos, plane_normal };

        Manifold manifold = doSATPlane(plane, collisionMeshA);

        if (manifold.numContactPoints > 0) {
            solver.addContacts({{
                b_entity, // Plane is always reference
                a_entity,
                {
                    manifold.contactPoints[0],
                    manifold.contactPoints[1],
                    manifold.contactPoints[2],
                    manifold.contactPoints[3],
                },
                manifold.numContactPoints,
                manifold.normal,
                0.f,
            }});
        }
    } break;
    default: __builtin_unreachable();
    }
}

}

namespace solver {

static inline Vector3 multDiag(Vector3 diag, Vector3 v)
{
    return Vector3 {
        diag.x * v.x,
        diag.y * v.y,
        diag.z * v.z,
    };
}

inline void substepRigidBodies(Context &ctx,
                               Position &pos,
                               Rotation &rot,
                               Velocity &vel,
                               const ObjectID &obj_id,
                               SubstepPrevState &prev_state,
                               SubstepStartState &start_state,
                               SubstepVelocityState &vel_state)
{
    const auto &solver = ctx.getSingleton<SolverData>();
    const ObjectManager &obj_mgr = *ctx.getSingleton<ObjectData>().mgr;
    const RigidBodyMetadata &metadata = obj_mgr.metadata[obj_id.idx];
    Vector3 inv_I = metadata.invInertiaTensor;
    float inv_m = metadata.invMass;

    float h = solver.h;

    Vector3 cur_position = pos;
    Quat cur_rotation = rot;

    prev_state.prevPosition = cur_position;
    prev_state.prevRotation = cur_rotation;

    Vector3 linear_velocity = vel.linear;
    Vector3 angular_velocity = vel.angular;

    vel_state.prevLinear = linear_velocity;
    vel_state.prevAngular = angular_velocity;

    // FIXME should really implement static objects differently:
    if (inv_m > 0) {
        linear_velocity += h * solver.g;
    }
 
    cur_position += h * linear_velocity;

    Vector3 I = {
        (inv_I.x == 0) ? 0.0f : 1.0f / inv_I.x,
        (inv_I.y == 0) ? 0.0f : 1.0f / inv_I.y,
        (inv_I.z == 0) ? 0.0f : 1.0f / inv_I.z
    };

    Vector3 torque_ext { 0, 0, 0 };
    Vector3 I_angular = multDiag(I, angular_velocity);

    angular_velocity +=
        h * multDiag(inv_I, torque_ext - (cross(angular_velocity, I_angular)));
    vel.angular = angular_velocity;

    Quat angular_quat = Quat::fromAngularVec(0.5f * h * angular_velocity);

    cur_rotation += angular_quat * cur_rotation;
    cur_rotation = cur_rotation.normalize();

    pos = cur_position;
    rot = cur_rotation;

    start_state.startPosition = cur_position;
    start_state.startRotation = cur_rotation;
}

static inline float generalizedInverseMass(Vector3 local,
                                           float inv_m,
                                           Vector3 inv_I,
                                           Vector3 n)
{
    Vector3 lxn = cross(local, n);
    return inv_m + dot(multDiag(inv_I, lxn), lxn);
}

template <typename Fn>
static MADRONA_ALWAYS_INLINE inline void applyPositionalUpdate(
    Vector3 &x1, Vector3 &x2,
    Quat &q1, Quat &q2,
    Vector3 r1, Vector3 r2,
    float inv_m1, float inv_m2,
    Vector3 inv_I1, Vector3 inv_I2,
    Vector3 n_world, Vector3 n1, Vector3 n2,
    float c,
    float alpha_tilde,
    float &lambda,
    Fn &&lambda_check)
{
    float w1 = generalizedInverseMass(r1, inv_m1, inv_I1, n1);
    float w2 = generalizedInverseMass(r2, inv_m2, inv_I2, n2);

    float delta_lambda =
        (-c - alpha_tilde * lambda) / (w1 + w2 + alpha_tilde);

    lambda += delta_lambda;

    if (lambda_check(lambda)) return;

    Vector3 p = delta_lambda * n_world;
    Vector3 p_local1 = delta_lambda * n1;
    Vector3 p_local2 = delta_lambda * n2;

    x1 += p * inv_m1;
    x2 -= p * inv_m2;

    Vector3 r1_x_p = cross(r1, p_local1);
    Vector3 r2_x_p = cross(r2, p_local2);

    q1 = q1 + Quat::fromAngularVec(0.5f * multDiag(inv_I1, r1_x_p)) * q1;
    q2 = q2 - Quat::fromAngularVec(0.5f * multDiag(inv_I2, r2_x_p)) * q2;

    // FIXME these normalizes aren't in the paper but seem necessary since
    // we immediately will use q1 and q2 after this
    q1 = q1.normalize();
    q2 = q2.normalize();
}

static MADRONA_ALWAYS_INLINE inline void handleContactConstraint(
    Vector3 &x1, Vector3 &x2,
    Quat &q1, Quat &q2,
    SubstepPrevState prev1, SubstepPrevState prev2,
    float inv_m1, float inv_m2,
    Vector3 inv_I1, Vector3 inv_I2,
    float mu_s1, float mu_s2,
    Vector3 r1, Vector3 r2,
    Vector3 n_world,
    float &lambda_n,
    float &lambda_t)
{
    Vector3 p1 = q1.rotateVec(r1) + x1;
    Vector3 p2 = q2.rotateVec(r2) + x2;

    float d = dot(p1 - p2, n_world);

    if (d <= 0) {
        return;
    }

    Vector3 x1_prev = prev1.prevPosition;
    Quat q1_prev = prev1.prevRotation;

    Vector3 x2_prev = prev2.prevPosition;
    Quat q2_prev = prev2.prevRotation;

    Vector3 p1_hat = q1_prev.rotateVec(r1) + x1_prev;
    Vector3 p2_hat = q2_prev.rotateVec(r2) + x2_prev;

    Vector3 n_local1 = q1.inv().rotateVec(n_world);
    Vector3 n_local2 = q2.inv().rotateVec(n_world);

    applyPositionalUpdate(x1, x2,
                          q1, q2,
                          r1, r2,
                          inv_m1, inv_m2,
                          inv_I1, inv_I2,
                          n_world, n_local1, n_local2,
                          d, 0,
                          lambda_n, [](float) { return false; });

    Vector3 delta_p = (p1 - p1_hat) - (p2 - p2_hat);
    Vector3 delta_p_t = delta_p - dot(delta_p, n_world) * n_world;

    float tangential_magnitude = delta_p_t.length();

    if (tangential_magnitude > 0.f) {
        Vector3 tangent_dir = delta_p_t / tangential_magnitude;
        Vector3 tangent_dir_local1 = q1.inv().rotateVec(tangent_dir);
        Vector3 tangent_dir_local2 = q2.inv().rotateVec(tangent_dir);

        float mu_s = 0.5f * (mu_s1 + mu_s2);
        float lambda_threshold = lambda_n * mu_s;

        applyPositionalUpdate(x1, x2,
                              q1, q2,
                              r1, r2,
                              inv_m1, inv_m2,
                              inv_I1, inv_I2,
                              tangent_dir, tangent_dir_local1, tangent_dir_local2,
                              tangential_magnitude,
                              0, lambda_t, [lambda_threshold](float lambda) {
                                  return lambda >= lambda_threshold;
                              });
    }
}

static inline MADRONA_ALWAYS_INLINE std::pair<Vector3, Vector3>
getLocalSpaceContacts(const SubstepStartState &start1,
                      const SubstepStartState &start2,
                      const Contact &contact,
                      CountT point_idx)
{
    Vector3 contact1 = contact.points[point_idx].xyz();
    float penetration_depth = contact.points[point_idx].w;

    Vector3 contact2 = 
        contact1 - contact.normal * penetration_depth;

    // Transform the contact points into local space for a & b
    Vector3 r1 = start1.startRotation.inv().rotateVec(
        contact1 - start1.startPosition);
    Vector3 r2 = start2.startRotation.inv().rotateVec(
        contact2 - start2.startPosition);

    return { r1, r2 };
}

// For now, this function assumes both a & b are dynamic objects.
// FIXME: Need to add dynamic / static variant or handle missing the velocity
// component for static objects.
static inline void handleContact(Context &ctx,
                                 ObjectManager &obj_mgr,
                                 Contact &contact)
{
    Position *p1_ptr = &ctx.getUnsafe<Position>(contact.ref);
    Rotation *q1_ptr = &ctx.getUnsafe<Rotation>(contact.ref);
    SubstepPrevState prev1 = ctx.getUnsafe<SubstepPrevState>(contact.ref);
    SubstepStartState start1 = ctx.getUnsafe<SubstepStartState>(contact.ref);
    ObjectID obj_id1 = ctx.getUnsafe<ObjectID>(contact.ref);
    RigidBodyMetadata metadata1 = obj_mgr.metadata[obj_id1.idx];

    Position *p2_ptr = &ctx.getUnsafe<Position>(contact.alt);
    Rotation *q2_ptr = &ctx.getUnsafe<Rotation>(contact.alt);
    SubstepPrevState prev2 = ctx.getUnsafe<SubstepPrevState>(contact.alt);
    SubstepStartState start2 = ctx.getUnsafe<SubstepStartState>(contact.alt);
    ObjectID obj_id2 = ctx.getUnsafe<ObjectID>(contact.alt);
    RigidBodyMetadata metadata2 = obj_mgr.metadata[obj_id2.idx];

    float lambda_n = 0.f;
    float lambda_t = 0.f;

    Vector3 p1 = *p1_ptr;
    Vector3 p2 = *p2_ptr;

    Quat q1 = *q1_ptr;
    Quat q2 = *q2_ptr;

    float inv_m1 = metadata1.invMass;
    float inv_m2 = metadata2.invMass;

    Vector3 inv_I1 = metadata1.invInertiaTensor;
    Vector3 inv_I2 = metadata2.invInertiaTensor;

    float mu_s1 = metadata1.muS;
    float mu_s2 = metadata2.muS;

#pragma unroll
    for (CountT i = 0; i < 4; i++) {
        if (i >= contact.numPoints) continue;

        auto [r1, r2] = getLocalSpaceContacts(start1, start2, contact, i);

        handleContactConstraint(p1, p2,
                                q1, q2,
                                prev1, prev2,
                                inv_m1, inv_m2,
                                inv_I1, inv_I2,
                                mu_s1, mu_s2,
                                r1, r2,
                                contact.normal,
                                lambda_n,
                                lambda_t);
    }

    *p1_ptr = p1;
    *p2_ptr = p2;

    *q1_ptr = q1;
    *q2_ptr = q2;

    contact.lambdaN = lambda_n;
}

inline void solvePositions(Context &ctx, SolverData &solver)
{
    ObjectManager &obj_mgr = *ctx.getSingleton<ObjectData>().mgr;

    // Push objects in serial based on the contact normal - total BS.
    CountT num_contacts = solver.numContacts.load(std::memory_order_relaxed);

    //printf("Solver # contacts: %d\n", num_contacts);

    for (CountT i = 0; i < num_contacts; i++) {
        Contact contact = solver.contacts[i];
        handleContact(ctx, obj_mgr, contact);

        solver.contacts[i].lambdaN = contact.lambdaN;
    }
}

inline void setVelocities(Context &ctx,
                          const Position &pos,
                          const Rotation &rot,
                          const SubstepPrevState &prev_state,
                          Velocity &vel)
{
    const auto &solver = ctx.getSingleton<SolverData>();
    float h = solver.h;

    vel.linear = (pos - prev_state.prevPosition) / h;

    Quat cur_rotation = rot;
    Quat prev_rotation = prev_state.prevRotation;

    Quat delta_q = cur_rotation * prev_rotation.inv();

    Vector3 new_angular = 2.f / h * Vector3 { delta_q.x, delta_q.y, delta_q.z };

    vel.angular = delta_q.w > 0.f ? new_angular : -new_angular;
}

static inline void applyVelocityUpdate(Vector3 &v1, Vector3 &v2,
                                       Vector3 &omega1, Vector3 &omega2,
                                       Vector3 r1, Vector3 r2,
                                       float inv_m1, float inv_m2,
                                       Vector3 inv_I1, Vector3 inv_I2,
                                       Vector3 delta_v_world,
                                       Vector3 delta_v_l1, Vector3 delta_v_l2,
                                       float delta_v_magnitude)
{
    float w1 = generalizedInverseMass(r1, inv_m1, inv_I1, delta_v_l1);
    float w2 = generalizedInverseMass(r2, inv_m2, inv_I2, delta_v_l2);

    delta_v_magnitude *= 1.f / (w1 + w2);

    v1 += delta_v_world * delta_v_magnitude * inv_m1;
    v2 -= delta_v_world * delta_v_magnitude * inv_m2;

    omega1 += multDiag(inv_I1, cross(r1, delta_v_l1 * delta_v_magnitude));
    omega2 -= multDiag(inv_I2, cross(r2, delta_v_l2 * delta_v_magnitude));
}

static inline void updateVelocityFromContact(Context &ctx,
                                             ObjectManager &obj_mgr,
                                             Contact contact,
                                             float h,
                                             float restitution_threshold)
{
    Velocity *v1_out = &ctx.getUnsafe<Velocity>(contact.ref);
    Quat q1 = ctx.getUnsafe<Rotation>(contact.ref);
    SubstepStartState start1 = ctx.getUnsafe<SubstepStartState>(contact.ref);
    SubstepVelocityState prev_vel1 = ctx.getUnsafe<SubstepVelocityState>(contact.ref);
    ObjectID obj_id1 = ctx.getUnsafe<ObjectID>(contact.ref);
    RigidBodyMetadata metadata1 = obj_mgr.metadata[obj_id1.idx];

    Velocity *v2_out = &ctx.getUnsafe<Velocity>(contact.alt);
    Quat q2 = ctx.getUnsafe<Rotation>(contact.alt);
    SubstepStartState start2 = ctx.getUnsafe<SubstepStartState>(contact.alt);
    SubstepVelocityState prev_vel2 = ctx.getUnsafe<SubstepVelocityState>(contact.alt);
    ObjectID obj_id2 = ctx.getUnsafe<ObjectID>(contact.alt);
    RigidBodyMetadata metadata2 = obj_mgr.metadata[obj_id2.idx];

    auto [v1, omega1] = *v1_out;
    auto [v2, omega2] = *v2_out;

    float mu_d = 0.5f * (metadata1.muD + metadata2.muD);

    // h * mu_d * |f_n| in paper
    float dynamic_friction_magnitude = mu_d * fabsf(contact.lambdaN) / h;
#pragma unroll
    for (CountT i = 0; i < 4; i++) {
        if (i >= contact.numPoints) continue;

        auto [r1, r2] = getLocalSpaceContacts(start1, start2, contact, i);
        Vector3 n = contact.normal;

        Vector3 v = (v1 + cross(omega1, r1)) - (v2 + cross(omega2, r2));

        float vn = dot(n, v);
        Vector3 vt = v - n * vn;

        float vt_len = vt.length();

        if (vt_len != 0 && dynamic_friction_magnitude != 0.f) {
            float corrected_magnitude =
                -fminf(dynamic_friction_magnitude, vt_len);

            Vector3 delta_world = vt / vt_len;

            Vector3 delta_local1 = q1.inv().rotateVec(delta_world);
            Vector3 delta_local2 = q2.inv().rotateVec(delta_world);

            applyVelocityUpdate(
                v1, v2,
                omega1, omega2,
                r1, r2,
                metadata1.invMass, metadata2.invMass,
                metadata1.invInertiaTensor, metadata2.invInertiaTensor,
                delta_world, delta_local1, delta_local2,
                corrected_magnitude);
        }

        Vector3 v_bar =
            (prev_vel1.prevLinear + cross(prev_vel1.prevAngular, r1)) -
            (prev_vel2.prevLinear + cross(prev_vel2.prevAngular, r2));

        float vn_bar = dot(n, v_bar);

        float e = 0.4; // FIXME
        if (fabsf(vn_bar) <= restitution_threshold) {
            e = 0.f;
        }
        float restitution_magnitude = (fminf(-e * vn_bar, 0) - vn);

        Vector3 n_local1 = q1.inv().rotateVec(n);
        Vector3 n_local2 = q2.inv().rotateVec(n);

        // FIXME: confirm this is pointing in right direction
        applyVelocityUpdate(
            v1, v2,
            omega1, omega2,
            r1, r2,
            metadata1.invMass, metadata2.invMass,
            metadata1.invInertiaTensor, metadata2.invInertiaTensor,
            n, n_local1, n_local2, restitution_magnitude);
    }

    *v1_out = Velocity { v1, omega1 };
    *v2_out = Velocity { v2, omega2 };
}

inline void solveVelocities(Context &ctx, SolverData &solver)
{
    ObjectManager &obj_mgr = *ctx.getSingleton<ObjectData>().mgr;

    CountT num_contacts = solver.numContacts.load(std::memory_order_relaxed);

    for (CountT i = 0; i < num_contacts; i++) {
        const Contact &contact = solver.contacts[i];
        updateVelocityFromContact(ctx, obj_mgr, contact, solver.h,
                                  solver.restitutionThreshold);
    }

    solver.numContacts.store(0, std::memory_order_relaxed);
}

}

void RigidBodyPhysicsSystem::init(Context &ctx,
                                  ObjectManager *obj_mgr,
                                  float delta_t,
                                  CountT num_substeps,
                                  math::Vector3 gravity,
                                  CountT max_dynamic_objects,
                                  CountT max_contacts_per_world)
{
    broadphase::BVH &bvh = ctx.getSingleton<broadphase::BVH>();
    new (&bvh) broadphase::BVH(max_dynamic_objects);

    SolverData &solver = ctx.getSingleton<SolverData>();
    new (&solver) SolverData(max_contacts_per_world, delta_t, num_substeps, gravity);

    ObjectData &objs = ctx.getSingleton<ObjectData>();
    new (&objs) ObjectData { obj_mgr };
}

void RigidBodyPhysicsSystem::reset(Context &ctx)
{
    broadphase::BVH &bvh = ctx.getSingleton<broadphase::BVH>();
    bvh.rebuildOnUpdate();
    bvh.clearLeaves();
}

broadphase::LeafID RigidBodyPhysicsSystem::registerEntity(Context &ctx,
                                                          Entity e)
{
    return ctx.getSingleton<broadphase::BVH>().reserveLeaf(e);
}

void RigidBodyPhysicsSystem::registerTypes(ECSRegistry &registry)
{
    registry.registerComponent<broadphase::LeafID>();
    registry.registerSingleton<broadphase::BVH>();

    registry.registerComponent<Velocity>();
    registry.registerComponent<CollisionAABB>();

    registry.registerComponent<solver::SubstepPrevState>();
    registry.registerComponent<solver::SubstepStartState>();
    registry.registerComponent<solver::SubstepVelocityState>();

    registry.registerComponent<CollisionEvent>();
    registry.registerArchetype<CollisionEventTemporary>();

    registry.registerComponent<CandidateCollision>();
    registry.registerArchetype<CandidateTemporary>();

    registry.registerSingleton<SolverData>();
    registry.registerSingleton<ObjectData>();

}

TaskGraph::NodeID RigidBodyPhysicsSystem::setupTasks(
    TaskGraph::Builder &builder, Span<const TaskGraph::NodeID> deps,
    CountT num_substeps)
{
    auto update_aabbs = builder.addToGraph<ParallelForNode<Context,
        updateCollisionAABB, Position, Rotation, ObjectID, Velocity,
            CollisionAABB>>(deps);

    auto preprocess_leaves = builder.addToGraph<ParallelForNode<Context,
        broadphase::updateLeavesEntry, broadphase::LeafID, 
        CollisionAABB>>({update_aabbs});

    auto bvh_update = builder.addToGraph<ParallelForNode<Context,
        broadphase::updateBVHEntry, broadphase::BVH>>({preprocess_leaves});

    auto find_overlapping = builder.addToGraph<ParallelForNode<Context,
        broadphase::findOverlappingEntry, Entity, CollisionAABB, Velocity>>(
            {bvh_update});
    
    auto cur_node = find_overlapping;
    for (CountT i = 0; i < num_substeps; i++) {
        auto rgb_update = builder.addToGraph<ParallelForNode<Context,
            solver::substepRigidBodies, Position, Rotation, Velocity, ObjectID,
            solver::SubstepPrevState, solver::SubstepStartState,
            solver::SubstepVelocityState>>({cur_node});

        auto run_narrowphase = builder.addToGraph<ParallelForNode<Context,
            narrowphase::runNarrowphase, CandidateCollision>>(
                {rgb_update});

        auto solve_pos = builder.addToGraph<ParallelForNode<Context,
            solver::solvePositions, SolverData>>({run_narrowphase});

        auto vel_set = builder.addToGraph<ParallelForNode<Context,
            solver::setVelocities, Position, Rotation,
            solver::SubstepPrevState, Velocity>>({solve_pos});

        auto solve_vel = builder.addToGraph<ParallelForNode<Context,
            solver::solveVelocities, SolverData>>({vel_set});

        cur_node = builder.addToGraph<ResetTmpAllocNode>({solve_vel});
    }

    auto clear_candidates = builder.addToGraph<
        ClearTmpNode<CandidateTemporary>>({cur_node});

    return clear_candidates;
}

TaskGraph::NodeID RigidBodyPhysicsSystem::setupCleanupTasks(
    TaskGraph::Builder &builder, Span<const TaskGraph::NodeID> deps)
{
    return builder.addToGraph<ClearTmpNode<CollisionEventTemporary>>(deps);
}

}
}

