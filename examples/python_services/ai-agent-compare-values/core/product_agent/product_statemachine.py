from statemachine import StateMachine, State

class ProductStateMachine(StateMachine):
    INIT = State('Init', initial=True)  # 初始状态，加载用户购物清单
    PLANNING = State('Planning')        # 规划阶段，规划产品搜索，搜索结果返回
    PROMOTE = State('Promote')          # 产品推广阶段，向用户推荐产品
    DETAIL = State('Detail')            # react模型框架，与用户交流产品具体细节，比价，分析
    DONE = State('Done')                # 产品任务完成，返回结果给用户
    FAILED = State('Failed')            # 产品任务失败，返回失败

    # 定义状态转换
    plan = INIT.to(PLANNING) | PLANNING.to(PLANNING)                      # 初始化加载完成后进入 PLANNING 状态
    promote = PLANNING.to(PROMOTE)                                        # 规划任务完成后进入 PROMOTE 状态
    detail = PROMOTE.to(DETAIL)                                           # 解决用户的提问扮演客服，帮助用户分析产品，进入 DETAIL 状态
    done = DETAIL.to(DONE)                                                # 向用户确认产品后，生成链接帮助用户下单
    fail = INIT.to(FAILED) | PLANNING.to(FAILED) | PROMOTE.to(FAILED) | DETAIL.to(FAILED)       
    reset = FAILED.to(INIT) | DONE.to(INIT)
    
    def __init__(self):
        self._before_plan_callback = None
        self._before_observe_callback = None
        self._before_detail_callback = None
        super().__init__()                                                     # 整体流程结束后重置状态

    def on_enter_INIT(self):
        print("进入 INIT")

    def on_enter_PLANNING(self):
        if self._before_plan_callback:
            self._before_plan_callback()
        print("进入 PLANNING")

    def on_enter_PROMOTE(self):
        # _before_observe 是异步的，已移到 ProductAgent.run() 中显式调用
        print("进入 PROMOTE")

    def on_enter_DETAIL(self):
        if self._before_detail_callback:
            self._before_detail_callback()