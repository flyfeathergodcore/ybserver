#include <coroutine>
#include <iostream>

struct Task{
    struct promise_type{
        Task get_return_object(){return {};}
        std::suspend_never initial_suspend(){return {};}
        std::suspend_never final_suspend()noexcept{return {};}
        void return_void(){}
        void unhandled_exception(){std::terminate();}
    };
};

Task hello(){
    std::cout<< " hello from coroutine" << std::endl;
    co_return;
}

int main(){
    hello();
    std::cout << "hello from main" << std::endl;
    return 0;
}