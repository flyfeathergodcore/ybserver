from statemachine import StateMachine, State

class GuideStateMachine(StateMachine):
    INIT = State('Init', initial=True)  # 初始状态，加载用户画像和欢迎词
    ASKING = State('Asking')            # 与用户交谈，确认产品或者场景
    OBSERVING = State('Observing')      # 产品具体细节打磨，设置提问上线和提问技巧，最终输出概括json
    DETAIL = State('Detail')            # 检查json是否达到标准，输出一次用户补充提问，加载之后补充json概括产品
    DONE = State('Done')                # 引导任务完成，转交产品agent
    FAILED = State('Failed')            # 引导任务失败，返回失败

    ask = INIT.to(ASKING)
    observe = ASKING.to(OBSERVING)
    detail = ASKING.to(DETAIL) | OBSERVING.to(DETAIL)
    done = ASKING.to(DONE) | OBSERVING.to(DONE) | DETAIL.to(DONE)
    fail = INIT.to(FAILED) | ASKING.to(FAILED) | OBSERVING.to(FAILED) | DETAIL.to(FAILED)
    reset = FAILED.to(INIT) | DONE.to(INIT)  # ✅ 现在合法了

    def on_enter_INIT(self):
        print("进入 INIT")

    def on_enter_ASKING(self):
        print("进入 ASKING")

    def on_enter_OBSERVING(self):
        print("进入 OBSERVING")

    def on_enter_DETAIL(self):
        print("进入 DETAIL")

    def on_enter_DONE(self):
        print("进入 DONE")

    def on_enter_FAILED(self):
        print("进入 FAILED")

if __name__ == "__main__":
    sm = GuideStateMachine()
    print(f"当前: {sm.current_state}")
    
    sm.ask()
    print(f"当前: {sm.current_state}")
    
    sm.observe()
    print(f"当前: {sm.current_state}")
    
    sm.detail()
    print(f"当前: {sm.current_state}")
    
    sm.done()
    print(f"当前: {sm.current_state}")
    
    sm.reset()
    print(f"重置后: {sm.current_state}")