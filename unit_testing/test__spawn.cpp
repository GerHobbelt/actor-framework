#define CPPA_VERBOSE_CHECK

#include <stack>
#include <chrono>
#include <iostream>
#include <functional>

#include "test.hpp"
#include "ping_pong.hpp"

#include "cppa/on.hpp"
#include "cppa/cppa.hpp"
#include "cppa/actor.hpp"
#include "cppa/factory.hpp"
#include "cppa/scheduler.hpp"
#include "cppa/sb_actor.hpp"
#include "cppa/to_string.hpp"
#include "cppa/exit_reason.hpp"
#include "cppa/event_based_actor.hpp"
#include "cppa/util/callable_trait.hpp"

using namespace std;
using namespace cppa;

class event_testee : public sb_actor<event_testee> {

    friend class sb_actor<event_testee>;

    behavior wait4string;
    behavior wait4float;
    behavior wait4int;

    behavior& init_state = wait4int;

 public:

    event_testee() {
        wait4string = (
            on<string>() >> [=]() {
                become(wait4int);
            },
            on<atom("get_state")>() >> [=]() {
                reply("wait4string");
            }
        );

        wait4float = (
            on<float>() >> [=]() {
                become(wait4string);
            },
            on<atom("get_state")>() >> [=]() {
                reply("wait4float");
            }
        );

        wait4int = (
            on<int>() >> [=]() {
                become(wait4float);
            },
            on<atom("get_state")>() >> [=]() {
                reply("wait4int");
            }
        );
    }

};

// quits after 5 timeouts
actor_ptr spawn_event_testee2() {
    struct impl : sb_actor<impl> {
        behavior wait4timeout(int remaining) {
            return (
                after(chrono::milliseconds(50)) >> [=]() {
                    if (remaining == 1) quit();
                    else become(wait4timeout(remaining - 1));
                }
            );
        }

        behavior init_state;

        impl() : init_state(wait4timeout(5)) { }
    };
    return spawn<impl>();
}

struct chopstick : public sb_actor<chopstick> {

    behavior taken_by(actor_ptr whom) {
        return (
            on<atom("take")>() >> [=]() {
                reply(atom("busy"));
            },
            on(atom("put"), whom) >> [=]() {
                become(available);
            },
            on(atom("break")) >> [=]() {
                quit();
            }
        );
    }

    behavior available;

    behavior& init_state = available;

    chopstick() {
        available = (
            on(atom("take"), arg_match) >> [=](actor_ptr whom) {
                become(taken_by(whom));
                reply(atom("taken"));
            },
            on(atom("break")) >> [=]() {
                quit();
            }
        );
    }

};

class testee_actor {

    void wait4string() {
        bool string_received = false;
        do_receive (
            on<string>() >> [&]() {
                string_received = true;
            },
            on<atom("get_state")>() >> [&]() {
                reply("wait4string");
            }
        )
        .until(gref(string_received));
    }

    void wait4float() {
        bool float_received = false;
        do_receive (
            on<float>() >> [&]() {
                float_received = true;
            },
            on<atom("get_state")>() >> [&]() {
                reply("wait4float");
            }
        )
        .until(gref(float_received));
        wait4string();
    }

 public:

    void operator()() {
        receive_loop (
            on<int>() >> [&]() {
                wait4float();
            },
            on<atom("get_state")>() >> [&]() {
                reply("wait4int");
            }
        );
    }

};

// receives one timeout and quits
void testee1() {
    receive (
        after(chrono::milliseconds(10)) >> []() { }
    );
}

void testee2(actor_ptr other) {
    self->link_to(other);
    send(other, uint32_t(1));
    receive_loop (
        on<uint32_t>() >> [](uint32_t sleep_time) {
            // "sleep" for sleep_time milliseconds
            receive(after(chrono::milliseconds(sleep_time)) >> []() {});
            //reply(sleep_time * 2);
        }
    );
}

void testee3(actor_ptr parent) {
    // test a delayed_send / delayed_reply based loop
    delayed_send(self, chrono::milliseconds(50), atom("Poll"));
    int polls = 0;
    receive_for(polls, 5) (
        on(atom("Poll")) >> [&]() {
            if (polls < 4) {
                delayed_reply(chrono::milliseconds(50), atom("Poll"));
            }
            send(parent, atom("Push"), polls);
        }
    );
}

template<class Testee>
string behavior_test(actor_ptr et) {
    string result;
    string testee_name = detail::to_uniform_name(typeid(Testee));
    send(et, 1);
    send(et, 2);
    send(et, 3);
    send(et, .1f);
    send(et, "hello " + testee_name);
    send(et, .2f);
    send(et, .3f);
    send(et, "hello again " + testee_name);
    send(et, "goodbye " + testee_name);
    send(et, atom("get_state"));
    receive (
        on_arg_match >> [&](const string& str) {
            result = str;
        },
        after(chrono::minutes(1)) >> [&]() {
        //after(chrono::seconds(2)) >> [&]() {
            throw runtime_error(testee_name + " does not reply");
        }
    );
    send(et, atom("EXIT"), exit_reason::user_defined);
    await_all_others_done();
    return result;
}

class fixed_stack : public sb_actor<fixed_stack> {

    friend class sb_actor<fixed_stack>;

    size_t max_size = 10;

    vector<int> data;

    behavior full;
    behavior filled;
    behavior empty;

    behavior& init_state = empty;

 public:

    fixed_stack(size_t max) : max_size(max)  {

        full = (
            on(atom("push"), arg_match) >> [=](int) { },
            on(atom("pop")) >> [=]() {
                reply(atom("ok"), data.back());
                data.pop_back();
                become(filled);
            }
        );

        filled = (
            on(atom("push"), arg_match) >> [=](int what) {
                data.push_back(what);
                if (data.size() == max_size)
                    become(full);
            },
            on(atom("pop")) >> [=]() {
                reply(atom("ok"), data.back());
                data.pop_back();
                if (data.empty())
                    become(empty);
            }
        );

        empty = (
            on(atom("push"), arg_match) >> [=](int what) {
                data.push_back(what);
                become(filled);
            },
            on(atom("pop")) >> [=]() {
                reply(atom("failure"));
            }
        );

    }

};

void echo_actor() {
    receive (
        others() >> []() {
            reply_tuple(self->last_dequeued());
        }
    );
}


struct simple_mirror : sb_actor<simple_mirror> {

    behavior init_state = (
        others() >> []() {
            reply_tuple(self->last_dequeued());
        }
    );

};

int main() {
    CPPA_TEST(test__spawn);

    CPPA_IF_VERBOSE(cout << "test send() ... " << flush);
    send(self, 1, 2, 3, true);
    receive(on(1, 2, 3, true) >> []() { });
    CPPA_IF_VERBOSE(cout << "... with empty message... " << flush);
    self << any_tuple{};
    receive(on() >> []() { });
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    self << any_tuple{};
    receive(on() >> []() { });

    CPPA_IF_VERBOSE(cout << "test receive with zero timeout ... " << flush);
    receive (
        others() >> []() {
            cerr << "WTF?? received: " << to_string(self->last_dequeued())
                 << endl;
        },
        after(chrono::seconds(0)) >> []() {
            // mailbox empty
        }
    );
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    auto mirror = spawn<simple_mirror>();

    CPPA_IF_VERBOSE(cout << "test mirror ... " << flush);
    send(mirror, "hello mirror");
    receive(on("hello mirror") >> []() { });
    send(mirror, atom("EXIT"), exit_reason::user_defined);
    CPPA_IF_VERBOSE(cout << "await ... " << endl);
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test echo actor ... " << flush);
    auto mecho = spawn(echo_actor);
    send(mecho, "hello echo");
    receive (
        on("hello echo") >> []() { },
        others() >> [] {
            cout << "UNEXPECTED: " << to_string(self->last_dequeued()) << endl;
        }
    );
    CPPA_IF_VERBOSE(cout << "await ... " << endl);
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test delayed_send() ... " << flush);
    delayed_send(self, chrono::seconds(1), 1, 2, 3);
    receive(on(1, 2, 3) >> []() { });
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test timeout ... " << flush);
    receive(after(chrono::seconds(1)) >> []() { });
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "testee1 ... " << flush);
    spawn(testee1);
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "event_testee2 ... " << flush);
    spawn_event_testee2();
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "chopstick ... " << flush);
    auto cstk = spawn<chopstick>();
    send(cstk, atom("take"), self);
    receive (
        on(atom("taken")) >> [&]() {
            send(cstk, atom("put"), self);
            send(cstk, atom("break"));
        }
    );
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test event-based factory ... " << flush);
    auto factory = factory::event_based([&](int* i, float*, string*) {
        self->become (
            on(atom("get_int")) >> [i]() {
                reply(*i);
            },
            on(atom("set_int"), arg_match) >> [i](int new_value) {
                *i = new_value;
            },
            on(atom("done")) >> []() {
                self->quit();
            }
        );
    });
    auto foobaz_actor = factory.spawn(23);
    send(foobaz_actor, atom("get_int"));
    send(foobaz_actor, atom("set_int"), 42);
    send(foobaz_actor, atom("get_int"));
    send(foobaz_actor, atom("done"));
    receive (
        on_arg_match >> [&](int value) {
            CPPA_CHECK_EQUAL(23, value);
        }
    );
    receive (
        on_arg_match >> [&](int value) {
            CPPA_CHECK_EQUAL(42, value);
        }
    );
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test fixed_stack ... " << flush);
    auto st = spawn<fixed_stack>(10);
    // push 20 values
    for (int i = 0; i < 20; ++i) send(st, atom("push"), i);
    // pop 20 times
    for (int i = 0; i < 20; ++i) send(st, atom("pop"));
    // expect 10 failure messages
    {
        int i = 0;
        receive_for(i, 10) (
            on(atom("failure")) >> []() { }
        );
    }
    // expect 10 {'ok', value} messages
    {
        vector<int> values;
        int i = 0;
        receive_for(i, 10) (
            on(atom("ok"), arg_match) >> [&](int value) {
                values.push_back(value);
            }
        );
        vector<int> expected{9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
        CPPA_CHECK(values == expected);
    }
    // terminate st
    send(st, atom("EXIT"), exit_reason::user_defined);
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test sync send/receive ... " << flush);
    auto sync_testee1 = spawn([]() {
        receive (
            on(atom("get")) >> []() {
                reply(42, 2);
            }
        );
    });
    send(self, 0, 0);
    auto handle = sync_send(sync_testee1, atom("get"));
    // wait for some time (until sync response arrived in mailbox)
    receive (after(chrono::milliseconds(50)) >> []() { });
    // enqueue async messages (must be skipped by receive_response)
    send(self, 42, 1);
    // must skip sync message
    receive (
        on(42, arg_match) >> [&](int i) {
            CPPA_CHECK_EQUAL(1, i);
        }
    );
    // must skip remaining async message
    receive_response (handle) (
        on_arg_match >> [&](int a, int b) {
            CPPA_CHECK_EQUAL(42, a);
            CPPA_CHECK_EQUAL(2, b);
        },
        others() >> [&]() {
            CPPA_ERROR("unexpected message");
        },
        after(chrono::seconds(10)) >> [&]() {
            CPPA_ERROR("timeout during receive_response");
        }
    );
    // dequeue remaining async. message
    receive (on(0, 0) >> []() { });
    // make sure there's no other message in our mailbox
    receive (
        others() >> [&]() {
            CPPA_ERROR("unexpected message");
        },
        after(chrono::seconds(0)) >> []() { }
    );
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    CPPA_IF_VERBOSE(cout << "test sync send with factory spawned actor ... " << flush);
    auto sync_testee_factory = factory::event_based(
        [&]() {
            self->become (
                on("hi") >> [&]() {
                    auto handle = sync_send(self->last_sender(), "whassup?");
                    self->handle_response(handle) (
                        on_arg_match >> [&](const string& str) {
                            CPPA_CHECK(self->last_sender() != nullptr);
                            CPPA_CHECK_EQUAL("nothing", str);
                            reply("goodbye!");
                            self->quit();
                        },
                        after(chrono::minutes(1)) >> []() {
                            cerr << "PANIC!!!!" << endl;
                            abort();
                        }
                    );
                },
                others() >> []() {
                    cerr << "UNEXPECTED: " << to_string(self->last_dequeued())
                         << endl;
                }

            );
        }
    );
    CPPA_CHECK(true);
    auto sync_testee = sync_testee_factory.spawn();
    self->monitor(sync_testee);
    send(sync_testee, "hi");
    receive (
        on("whassup?") >> [&] {
            CPPA_CHECK(true);
            // this is NOT a reply, it's just an asynchronous message
            send(self->last_sender(), "a lot!");
            reply("nothing");
        }
    );
    receive (
        on("goodbye!") >> [&] {
            CPPA_CHECK(true);
        }
    );
    CPPA_CHECK(true);
    receive (
        on(atom("DOWN"), exit_reason::normal) >> [&] {
            CPPA_CHECK(self->last_sender() == sync_testee);
        }
    );
    await_all_others_done();
    CPPA_IF_VERBOSE(cout << "ok" << endl);

    sync_send(sync_testee, "!?").await(
        on(atom("EXITED"), any_vals) >> [&] {
            CPPA_CHECK(true);
        },
        others() >> [&] {
            CPPA_ERROR("'sync_testee' still alive?; received: "
                       << to_string(self->last_dequeued()));
        },
        after(chrono::milliseconds(5)) >> [&] {
            CPPA_CHECK(false);
        }
    );

    auto inflater = factory::event_based(
        [](string*, actor_ptr* receiver) {
            self->become(
                on_arg_match >> [=](int n, const string& s) {
                    send(*receiver, n * 2, s);
                },
                on(atom("done")) >> []() {
                    self->quit();
                }
            );
        }
    );
    auto joe = inflater.spawn("Joe", self);
    auto bob = inflater.spawn("Bob", joe);
    send(bob, 1, "hello actor");
    receive (
        on(4, "hello actor") >> [&] {
            CPPA_CHECK(true);
            // done
        },
        others() >> [&] {
            CPPA_ERROR("unexpected result");
            cerr << "unexpected: " << to_string(self->last_dequeued()) << endl;
        }
    );
    // kill joe and bob
    auto poison_pill = make_any_tuple(atom("done"));
    joe << poison_pill;
    bob << poison_pill;
    await_all_others_done();

    function<actor_ptr (const string&, const actor_ptr&)> spawn_next;
    auto kr34t0r = factory::event_based(
        // it's safe to pass spawn_next as reference here, because
        // - it is guaranteeed to outlive kr34t0r by general scoping rules
        // - the lambda is always executed in the current actor's thread
        // but using spawn_next in a message handler could
        // still cause undefined behavior!
        [&spawn_next](string* name, actor_ptr* pal) {
            if (*name == "Joe" && !*pal) {
                *pal = spawn_next("Bob", self);
            }
            self->become (
                others() >> [pal]() {
                    // forward message and die
                    *pal << self->last_dequeued();
                    self->quit();
                }
            );
        }
    );
    spawn_next = [&kr34t0r](const string& name, const actor_ptr& pal) {
        return kr34t0r.spawn(name, pal);
    };
    auto joe_the_second = kr34t0r.spawn("Joe");
    send(joe_the_second, atom("done"));
    await_all_others_done();

    int zombie_init_called = 0;
    int zombie_on_exit_called = 0;
    factory::event_based([&]() {
        ++zombie_init_called;
    },
    [&]() {
        ++zombie_on_exit_called;
    })
    .spawn();
    CPPA_CHECK_EQUAL(1, zombie_init_called);
    CPPA_CHECK_EQUAL(1, zombie_on_exit_called);
    factory::event_based([&](int* i) {
        CPPA_CHECK_EQUAL(42, *i);
        ++zombie_init_called;
    },
    [&](int* i) {
        CPPA_CHECK_EQUAL(42, *i);
        ++zombie_on_exit_called;
    })
    .spawn(42);
    CPPA_CHECK_EQUAL(2, zombie_init_called);
    CPPA_CHECK_EQUAL(2, zombie_on_exit_called);
    factory::event_based([&](int* i) {
        CPPA_CHECK_EQUAL(23, *i);
        ++zombie_init_called;
    },
    [&]() {
        ++zombie_on_exit_called;
    })
    .spawn(23);
    CPPA_CHECK_EQUAL(3, zombie_init_called);
    CPPA_CHECK_EQUAL(3, zombie_on_exit_called);

    auto f = factory::event_based([](string* name) {
        self->become (
            on(atom("get_name")) >> [name]() {
                reply(atom("name"), *name);
            }
        );
    });
    auto a1 = f.spawn("alice");
    auto a2 = f.spawn("bob");
    send(a1, atom("get_name"));
    receive (
        on(atom("name"), arg_match) >> [&](const string& name) {
            CPPA_CHECK_EQUAL("alice", name);
        }
    );
    send(a2, atom("get_name"));
    receive (
        on(atom("name"), arg_match) >> [&](const string& name) {
            CPPA_CHECK_EQUAL("bob", name);
        }
    );
    auto kill_msg = make_any_tuple(atom("EXIT"), exit_reason::user_defined);
    a1 << kill_msg;
    a2 << kill_msg;
    await_all_others_done();

    factory::event_based([](int* i) {
        self->become(
            after(chrono::milliseconds(50)) >> [=]() {
                if (++(*i) >= 5) self->quit();
            }

        );
    }).spawn();
    await_all_others_done();

    auto res1 = behavior_test<testee_actor>(spawn(testee_actor{}));
    CPPA_CHECK_EQUAL(res1, "wait4int");
    CPPA_CHECK_EQUAL(behavior_test<event_testee>(spawn<event_testee>()), "wait4int");

    // create 20,000 actors linked to one single actor
    // and kill them all through killing the link
    auto twenty_thousand = spawn([]() {
        for (int i = 0; i < 20000; ++i) {
            self->link_to(spawn<event_testee>());
        }
        receive_loop (
            others() >> []() {
                cout << "wtf? => " << to_string(self->last_dequeued()) << endl;
            }
        );
    });
    send(twenty_thousand, atom("EXIT"), exit_reason::user_defined);
    await_all_others_done();
    self->trap_exit(true);
    auto ping_actor = spawn(ping, 10);
    auto pong_actor = spawn(pong, ping_actor);
    self->monitor(pong_actor);
    self->monitor(ping_actor);
    self->link_to(pong_actor);
    int i = 0;
    int flags = 0;
    delayed_send(self, chrono::seconds(1), atom("FooBar"));
    // wait for DOWN and EXIT messages of pong
    receive_for(i, 4) (
        on<atom("EXIT"), uint32_t>() >> [&](uint32_t reason) {
            CPPA_CHECK_EQUAL(reason, exit_reason::user_defined);
            CPPA_CHECK(self->last_sender() == pong_actor);
            flags |= 0x01;
        },
        on<atom("DOWN"), uint32_t>() >> [&](uint32_t reason) {
            auto who = self->last_sender();
            if (who == pong_actor) {
                flags |= 0x02;
                CPPA_CHECK_EQUAL(reason, exit_reason::user_defined);
            }
            else if (who == ping_actor) {
                flags |= 0x04;
                CPPA_CHECK_EQUAL(reason, exit_reason::normal);
            }
        },
        on_arg_match >> [&](const atom_value& val) {
            CPPA_CHECK(val == atom("FooBar"));
            flags |= 0x08;
        },
        others() >> [&]() {
            CPPA_ERROR("unexpected message: " << to_string(self->last_dequeued()));
        },
        after(chrono::seconds(5)) >> [&]() {
            CPPA_ERROR("timeout in file " << __FILE__ << " in line " << __LINE__);
        }
    );
    // wait for termination of all spawned actors
    await_all_others_done();
    CPPA_CHECK_EQUAL(0x0F, flags);
    // verify pong messages
    CPPA_CHECK_EQUAL(10, pongs());
    return CPPA_TEST_RESULT;
}
