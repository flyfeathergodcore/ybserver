from statemachine import StateMachine, State

class GuideStateMachine(StateMachine):
    INIT = State('Init', initial=True)  # 初始状态，加载用户画像和欢迎词
    ASKING = State('Asking')            # 与用户交谈，确认产品或者场景
    OBSERVING = State('Observing')      # 产品具体细节打磨，设置提问上线和提问技巧，最终输出概括json
    DETAIL = State('Detail')            # 检查json是否达到标准，输出一次用户补充提问，加载之后补充json概括产品
    DONE = State('Done')                # 引导任务完成，转交产品agent
    FAILED = State('Failed')            # 引导任务失败，返回失败

    # 定义状态转换
    ask = INIT.to(ASKING) | ASKING.to(ASKING)                                                      # 初始化加载完成后进入 ASKING 状态
    observe = ASKING.to(OBSERVING)                                                              # 用户确认产品或场景后进入 OBSERVING 状态
    detail = ASKING.to(DETAIL) | OBSERVING.to(DETAIL)                                           # 明确产品品类和产品核心需求key或者用户中断讨论后进入 DETAIL 状态
    done = DETAIL.to(DONE)                                                                      # 向用户确认产品或场景后，结合补充信息(用户补充和历史分析补充)进入 DONE 状态
    fail = INIT.to(FAILED) | ASKING.to(FAILED) | OBSERVING.to(FAILED) | DETAIL.to(FAILED)       
    reset = FAILED.to(INIT) | DONE.to(INIT)
    
    def __init__(self):
        self._before_ask_callback = None
        self._before_observe_callback = None
        self._before_detail_callback = None
        super().__init__()                                                     # 整体流程结束后重置状态

    def on_enter_INIT(self):
        print("进入 INIT")

    def on_enter_ASKING(self):
        if self._before_ask_callback:
            self._before_ask_callback()
        print("进入 ASKING")

    def on_enter_OBSERVING(self):
        # _before_observe 是异步的，已移到 GuideAgent.run() 中显式调用
        print("进入 OBSERVING")

    def on_enter_DETAIL(self):
        if self._before_detail_callback:
            self._before_detail_callback()
        print("进入 DETAIL")

    def on_enter_DONE(self):
        print("进入 DONE")

    def on_enter_FAILED(self):
        print("进入 FAILED")
    

if __name__ == "__main__":
    sm = GuideStateMachine()
    print(f"当前: {sm.current_state_value.name}")

    sm.ask()
    print(f"当前: {sm.current_state_value.name}")

    sm.observe()
    print(f"当前: {sm.current_state_value.name}")

    sm.detail()
    print(f"当前: {sm.current_state_value.name}")

    sm.done()
    print(f"当前: {sm.current_state_value.name}")

    sm.reset()
    print(f"重置后: {sm.current_state_value.name}")