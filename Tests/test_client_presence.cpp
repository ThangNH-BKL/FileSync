#include "framework.h"
#include "MiniTest.h"
#include "ClientPresence.h"

using fsyn::ClientPresence;

TEST(client_presence_connect_disconnect_reconnect) {
    ClientPresence statuses;
    statuses.Reconcile({{"192.168.0.97\\ABC", "192.168.0.97"}});
    EXPECT_EQ(statuses.Entries().size(), size_t(1));
    EXPECT_TRUE(statuses.Entries()[0].online);
    statuses.Reconcile({});
    EXPECT_EQ(statuses.Entries().size(), size_t(1));
    EXPECT_FALSE(statuses.Entries()[0].online);
    statuses.Reconcile({{"192.168.0.97\\ABC", "192.168.0.97"}});
    EXPECT_EQ(statuses.Entries().size(), size_t(1));
    EXPECT_TRUE(statuses.Entries()[0].online);
}

TEST(client_presence_same_ip_distinct_client_folders) {
    ClientPresence statuses;
    statuses.Reconcile({
        {"192.168.0.97\\Mini", "192.168.0.97"},
        {"192.168.0.97\\Main", "192.168.0.97"}
    });
    EXPECT_EQ(statuses.Entries().size(), size_t(2));
    statuses.Reconcile({{"192.168.0.97\\Main", "192.168.0.97"}});
    EXPECT_FALSE(statuses.Entries()[0].online);
    EXPECT_TRUE(statuses.Entries()[1].online);
}

TEST(client_presence_ignore_unverified_and_duplicate_sessions) {
    ClientPresence statuses;
    statuses.Reconcile({{"", "192.168.0.97"},
                        {"192.168.0.97\\ABC", "192.168.0.97"},
                        {"192.168.0.97\\ABC", "192.168.0.97"}});
    EXPECT_EQ(statuses.Entries().size(), size_t(1));
    EXPECT_TRUE(statuses.Entries()[0].online);
}

TEST(client_presence_blink_index) {
    EXPECT_EQ(ClientPresence::IconIndex(true, true), 0);
    EXPECT_EQ(ClientPresence::IconIndex(true, false), 1);
    EXPECT_EQ(ClientPresence::IconIndex(false, true), 2);
    EXPECT_EQ(ClientPresence::IconIndex(false, false), 3);
}
