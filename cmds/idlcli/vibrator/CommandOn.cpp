/*
 * Copyright (C) 2019 The Android Open Source Project *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <thread>

#include "utils.h"
#include "vibrator.h"

using std::chrono::milliseconds;
using std::this_thread::sleep_for;

namespace android {
namespace idlcli {

class CommandVibrator;

namespace vibrator {

class CommandOn : public Command {
    std::string getDescription() const override { return "Turn on vibrator."; }

    std::string getUsageSummary() const override { return "[options] <duration>"; }

    UsageDetails getUsageDetails() const override {
        UsageDetails details{
                {"-b", {"Block for duration of vibration."}},
                {"<duration>", {"In milliseconds."}},
        };
        return details;
    }

    Status doArgs(Args &args) override {
        while (args.get<std::string>().value_or("").find("-") == 0) {
            auto opt = *args.pop<std::string>();
            if (opt == "--") {
                break;
            } else if (opt == "-b") {
                mBlocking = true;
            } else {
                std::cerr << "Invalid Option '" << opt << "'!" << std::endl;
                return USAGE;
            }
        }
        if (auto duration = args.pop<decltype(mDuration)>()) {
            mDuration = *duration;
        } else {
            std::cerr << "Missing or Invalid Duration!" << std::endl;
            return USAGE;
        }
        if (!args.empty()) {
            std::cerr << "Unexpected Arguments!" << std::endl;
            return USAGE;
        }
        return OK;
    }

    Status doMain(Args && /*args*/) override {
        std::string statusStr;
        Status ret;
        std::shared_ptr<VibratorCallback> callback;

        if (auto hal = getHal<aidl::IVibrator>()) {
            ABinderProcess_setThreadPoolMaxThreadCount(1);
            ABinderProcess_startThreadPool();

            int32_t cap;
            hal->call(&aidl::IVibrator::getCapabilities, &cap);

            if (mBlocking && (cap & aidl::IVibrator::CAP_ON_CALLBACK)) {
                callback = ndk::SharedRefBase::make<VibratorCallback>();
            }

            auto status = hal->call(&aidl::IVibrator::on, mDuration, callback);

            statusStr = status.getDescription();
            ret = status.isOk() ? OK : ERROR;
        } else if (auto hal = getHal<V1_0::IVibrator>()) {
            auto status = hal->call(&V1_0::IVibrator::on, mDuration);
            statusStr = toString(status);
            ret = status.isOk() && status == V1_0::Status::OK ? OK : ERROR;
        } else {
            return UNAVAILABLE;
        }

        if (ret == OK && mBlocking) {
            if (callback) {
                callback->waitForComplete();
            } else {
                sleep_for(milliseconds(mDuration));
            }
        }

        std::cout << "Status: " << statusStr << std::endl;

        return ret;
    }

    bool mBlocking;
    uint32_t mDuration;
};

static const auto Command = CommandRegistry<CommandVibrator>::Register<CommandOn>("on");

} // namespace vibrator
} // namespace idlcli
} // namespace android
