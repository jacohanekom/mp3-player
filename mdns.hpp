#pragma once
/**
 * mdns.hpp – Minimal mDNS/DNS-SD service announcer via the Avahi client
 * library.
 *
 * Publishes one or more TCP services on the LAN (e.g. "_mp3player._tcp")
 * so the device can be found without knowing its IP — `avahi-browse -rt
 * _mp3player._tcp` or any Bonjour/Zeroconf-aware client will see it.
 *
 * Runs its own Avahi client + poll loop on a background thread. If
 * avahi-daemon isn't installed/running, avahi_client_new() fails, a
 * warning is logged once, and the rest of the program is unaffected —
 * this is a discovery convenience, not a required dependency.
 */
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

class MdnsAnnouncer {
public:
    struct Service {
        std::string type;  // e.g. "_mp3player._tcp"
        uint16_t    port;
    };

    MdnsAnnouncer(std::string instance_name, std::vector<Service> services)
        : instance_name_(std::move(instance_name)), services_(std::move(services)) {}

    ~MdnsAnnouncer() { stop(); }

    void start() {
        poll_ = avahi_simple_poll_new();
        if (!poll_) {
            std::cerr << "[mDNS] avahi_simple_poll_new failed\n";
            return;
        }
        int error = 0;
        client_ = avahi_client_new(avahi_simple_poll_get(poll_), static_cast<AvahiClientFlags>(0),
                                    &MdnsAnnouncer::clientCallback, this, &error);
        if (!client_) {
            std::cerr << "[mDNS] avahi_client_new failed: " << avahi_strerror(error)
                       << " (is avahi-daemon running? continuing without mDNS)\n";
            avahi_simple_poll_free(poll_);
            poll_ = nullptr;
            return;
        }
        thread_ = std::thread([this] { avahi_simple_poll_loop(poll_); });
    }

    void stop() {
        if (poll_) avahi_simple_poll_quit(poll_);
        if (thread_.joinable()) thread_.join();
        if (client_) { avahi_client_free(client_); client_ = nullptr; }  // also frees the entry group
        if (poll_)   { avahi_simple_poll_free(poll_); poll_ = nullptr; }
    }

private:
    void createServices(AvahiClient *client) {
        if (!group_) {
            group_ = avahi_entry_group_new(client, &MdnsAnnouncer::entryGroupCallback, this);
            if (!group_) {
                std::cerr << "[mDNS] avahi_entry_group_new failed: "
                           << avahi_strerror(avahi_client_errno(client)) << "\n";
                return;
            }
        }
        if (!avahi_entry_group_is_empty(group_)) return;

        for (const auto &svc : services_) {
            int ret = avahi_entry_group_add_service(
                group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0),
                instance_name_.c_str(), svc.type.c_str(), nullptr, nullptr, svc.port, nullptr);
            if (ret == AVAHI_ERR_COLLISION) { renameAndRetry(client); return; }
            if (ret < 0) {
                std::cerr << "[mDNS] add_service(" << svc.type << ") failed: " << avahi_strerror(ret) << "\n";
                return;
            }
        }

        int ret = avahi_entry_group_commit(group_);
        if (ret < 0) std::cerr << "[mDNS] entry_group_commit failed: " << avahi_strerror(ret) << "\n";
    }

    void renameAndRetry(AvahiClient *client) {
        char *n = avahi_alternative_service_name(instance_name_.c_str());
        instance_name_ = n;
        avahi_free(n);
        std::cerr << "[mDNS] Name collision, retrying as '" << instance_name_ << "'\n";
        avahi_entry_group_reset(group_);
        createServices(client);
    }

    static void clientCallback(AvahiClient *client, AvahiClientState state, void *userdata) {
        auto *self = static_cast<MdnsAnnouncer *>(userdata);
        switch (state) {
            case AVAHI_CLIENT_S_RUNNING:
                self->createServices(client);
                break;
            case AVAHI_CLIENT_FAILURE:
                std::cerr << "[mDNS] client failure: " << avahi_strerror(avahi_client_errno(client)) << "\n";
                avahi_simple_poll_quit(self->poll_);
                break;
            case AVAHI_CLIENT_S_COLLISION:
            case AVAHI_CLIENT_S_REGISTERING:
                if (self->group_) avahi_entry_group_reset(self->group_);
                break;
            case AVAHI_CLIENT_CONNECTING:
                break;
        }
    }

    static void entryGroupCallback(AvahiEntryGroup *group, AvahiEntryGroupState state, void *userdata) {
        auto *self = static_cast<MdnsAnnouncer *>(userdata);
        switch (state) {
            case AVAHI_ENTRY_GROUP_ESTABLISHED:
                std::cerr << "[mDNS] Announced as '" << self->instance_name_ << "'\n";
                break;
            case AVAHI_ENTRY_GROUP_COLLISION:
                self->renameAndRetry(avahi_entry_group_get_client(group));
                break;
            case AVAHI_ENTRY_GROUP_FAILURE:
                std::cerr << "[mDNS] entry group failure: "
                           << avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group))) << "\n";
                break;
            case AVAHI_ENTRY_GROUP_UNCOMMITED:
            case AVAHI_ENTRY_GROUP_REGISTERING:
                break;
        }
    }

    std::string          instance_name_;
    std::vector<Service> services_;
    AvahiSimplePoll      *poll_   = nullptr;
    AvahiClient          *client_ = nullptr;
    AvahiEntryGroup      *group_  = nullptr;
    std::thread          thread_;
};
