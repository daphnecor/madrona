#include <madrona/viz/system.hpp>
#include <madrona/components.hpp>

#include "interop.hpp"

namespace madrona::viz {
using namespace base;
using namespace math;

struct ViewerSystemState {
    PerspectiveCameraData *views;
    uint32_t *numViews;
    InstanceData *instances;
    uint32_t *numInstances;
    float aspectRatio;
};


inline void clearInstanceCount(Context &,
                               const ViewerSystemState &sys_state)
{
    *(sys_state.numInstances) = 0;
}

inline void instanceTransformSetup(Context &ctx,
                                   const Position &pos,
                                   const Rotation &rot,
                                   const Scale &scale,
                                   const ObjectID &obj_id)
{
    auto &sys_state = ctx.singleton<ViewerSystemState>();

    AtomicU32Ref inst_count_atomic(*sys_state.numInstances);
    uint32_t inst_idx = inst_count_atomic.fetch_add_relaxed(1);

    sys_state.instances[inst_idx] = InstanceData {
        pos,
        rot,
        scale,
        obj_id.idx,
        0,
    };
}


inline void updateViewData(Context &ctx,
                           const Position &pos,
                           const Rotation &rot,
                           const VizCamera &viz_cam)
{
    auto &sys_state = ctx.singleton<ViewerSystemState>();
    int32_t view_idx = viz_cam.viewIDX;

    Vector3 camera_pos = pos + viz_cam.cameraOffset;

    sys_state.views[view_idx] = PerspectiveCameraData {
        camera_pos,
        rot.inv(),
        viz_cam.xScale,
        viz_cam.yScale,
        viz_cam.zNear,
        {},
    };
}

#ifdef MADRONA_GPU_MODE

inline void readbackCount(Context &ctx,
                          RendererState &renderer_state)
{
    if (ctx.worldID().idx == 0) {
        *renderer_state.count_readback = renderer_state.numInstances->primitiveCount;
        renderer_state.numInstances->primitiveCount = 0;
    }
}

#endif

void VizRenderingSystem::registerTypes(ECSRegistry &registry)
{
    registry.registerComponent<VizCamera>();
    registry.registerSingleton<ViewerSystemState>();
}

TaskGraph::NodeID VizRenderingSystem::setupTasks(
    TaskGraph::Builder &builder,
    Span<const TaskGraph::NodeID> deps)
{
    // FIXME: It feels like we should have persistent slots for renderer
    // state rather than needing to continually reset the instance count
    // and recreate the buffer. However, this might be hard to handle with
    // double buffering
    auto instance_clear = builder.addToGraph<ParallelForNode<Context,
        clearInstanceCount,
        ViewerSystemState>>(deps);

    auto instance_setup = builder.addToGraph<ParallelForNode<Context,
        instanceTransformSetup,
        Position,
        Rotation,
        Scale,
        ObjectID>>({instance_clear});

    auto viewdata_update = builder.addToGraph<ParallelForNode<Context,
        updateViewData,
        Position,
        Rotation,
        VizCamera>>({instance_setup});

#ifdef MADRONA_GPU_MODE
    auto readback_count = builder.addToGraph<ParallelForNode<Context,
        readbackCount,
        ViewerSystemState>>({viewdata_update});

    return readback_count;
#else
    return viewdata_update;
#endif
}

void VizRenderingSystem::reset(Context &ctx)
{
    auto &system_state = ctx.singleton<ViewerSystemState>();
    *system_state.numViews = 0;
}

void VizRenderingSystem::init(Context &ctx,
                              const VizECSBridge *bridge)
{
    auto &system_state = ctx.singleton<ViewerSystemState>();

    int32_t world_idx = ctx.worldID().idx;

    system_state.views = bridge->views[world_idx];
    system_state.numViews = &bridge->numViews[world_idx];
    system_state.instances = bridge->instances[world_idx];
    system_state.numInstances = &bridge->numInstances[world_idx];
    system_state.aspectRatio = 
        (float)bridge->renderWidth / (float)bridge->renderHeight;
}

VizCamera VizRenderingSystem::setupView(
    Context &ctx,
    float vfov_degrees,
    float z_near,
    math::Vector3 camera_offset,
    int32_t view_idx)
{
    auto &sys_state = ctx.singleton<ViewerSystemState>();

    float fov_scale = tanf(toRadians(vfov_degrees * 0.5f));

    (*sys_state.numViews) += 1;

    float x_scale = fov_scale / sys_state.aspectRatio;
    float y_scale = -fov_scale;

    return VizCamera {
        x_scale,
        y_scale,
        z_near,
        camera_offset,
        view_idx,
    };
}

void VizRenderingSystem::markEpisode(Context &ctx)
{
}

}
