#ifdef LEROBOT_VANE_DISTRIBUTED
#include "vane/lerobot_task_compat.hpp"
#define ExecutorTask LerobotVaneProducerTask
#define ExecuteTask  ExecuteProducerTask
#define Reschedule() Reschedule(producer->CurrentInterruptEpoch())
#include "../cpp/test_video_producer.cpp"
#undef Reschedule
#undef ExecuteTask
#undef ExecutorTask
#endif
