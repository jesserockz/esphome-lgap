#include "lgap.h"
#include "lgap_device.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cinttypes>
#include <vector>

namespace esphome
{
  namespace lgap
  {
    float LGAP::get_setup_priority() const { return setup_priority::DATA; }

    void LGAP::dump_config()
    {
      ESP_LOGCONFIG(TAG, "LGAP:");
      check_uart_settings(4800);

      ESP_LOGCONFIG(TAG, "  Flow Control Pin:");
      if (this->flow_control_pin_ != null)
      {
        this->flow_control_pin_->dump_summary();
      }
      else
      {
        ESP_LOGCONFIG(TAG, "Flow control pin not set.");
      }

      ESP_LOGCONFIG(TAG, "  Send Wait Time: %dms", this->send_wait_time_);
      ESP_LOGCONFIG(TAG, "  Receive Wait Time: %dms", this->receive_wait_time_);
      ESP_LOGCONFIG(TAG, "  Zone Check Wait Time: %dms", this->zone_check_wait_time_);
      ESP_LOGCONFIG(TAG, "  Child devices: %d", this->devices_.size());
      if (this->debug_ == true)
      {
        ESP_LOGCONFIG(TAG, "  Debug: true");
        ESP_LOGCONFIG(TAG, "  Debug wait time: %dms", this->debug_wait_time_);
      }
    }

    // the checksum method is the same as the LG wall controller
    // borrowed this checksum function from:
    // https://github.com/JanM321/esphome-lg-controller/blob/998b78a212f798267feca0a91475726516228b56/esphome/lg-controller.h#L631C1-L637C6
    uint8_t LGAP::calculate_checksum(const std::vector<uint8_t> &data)
    {
      size_t result = 0;
      for (size_t i = 0; i < data.size() - 1; i++)
      {
        result += data[i];
      }
      return (result & 0xff) ^ 0x55;
    }

    void LGAP::loop()
    {
      const uint32_t now = millis();

      // do nothing if there are no LGAP devices registered
      if (this->devices_.size() == 0)
        return;

      // enable wait time between loops
      if ((now - this->last_loop_time_) < this->loop_wait_time_)
        return;
      else
        this->last_loop_time_ = now;

      // state == 0 - handle potential writes before reads
      if (this->state_ == 0)
      {
        if (this->debug_ == true)
          ESP_LOGV(TAG, "State: 0");

        // 1. check if anything needs to be sent right now
        for (auto &device : this->devices_)
        {
          ESP_LOGV(TAG, "Checking device %d for pending writes...", device->zone_number);

          if (device->zone_number > -1 && device->write_update_pending == true)
          {
            ESP_LOGV(TAG, "write_update_pending==true for zone %d", device->zone_number);
            this->tx_buffer_.clear();
            device->write_update_pending = false;

            // generate payload to send
            device->generate_lgap_request(*&this->tx_buffer_);

            if (this->flow_control_pin_ != nullptr)
              this->flow_control_pin_->digital_write(true);

            // don't allow writes of more than 8 bytes
            int bytes_to_write = this->tx_buffer_.size() >= 8 ? 8 : this->tx_buffer_.size();
            ESP_LOGV(TAG, "Writing %d bytes", bytes_to_write);
            this->write_array(this->tx_buffer_.data(), bytes_to_write);

            // write request to uart
            this->flush();
            this->tx_buffer_.clear();
            ESP_LOGV(TAG, "flushed write buffer.");

            if (this->flow_control_pin_ != nullptr)
              this->flow_control_pin_->digital_write(false);

            this->last_send_time_ = millis();
            // this->last_receive_time_ = this->last_send_time_;
            this->state_ = 1;
            continue;
          }
          else
          {
            ESP_LOGV(TAG, "write_update_pending==false for zone %d", device->zone_number);
          }
        }

        // 2. read only request for next zone status
        if (this->state_ == 0 && (now - this->last_zone_check_time_) > zone_check_wait_time_)
        {
          this->last_zone_check_time_ = millis();
          if (this->debug_ == true)
            ESP_LOGV(TAG, "Checking next zone");

          // cycle through zones
          this->last_zone_checked_index_ = (this->last_zone_checked_index_ + 1) > this->devices_.size() - 1 ? 0 : this->last_zone_checked_index_ + 1;
          if (this->debug_ == true)
            ESP_LOGV(TAG, "Checking zone index %d -> to zone number %d", this->last_zone_checked_index_, this->devices_[this->last_zone_checked_index_]->zone_number);

          // retrieve lgap message from device if it has a valid zone number
          if (this->devices_[this->last_zone_checked_index_]->zone_number > -1)
          {
            ESP_LOGV(TAG, "LGAP requesting update from zone %d", this->devices_[this->last_zone_checked_index_]->zone_number);

            this->tx_buffer_.clear();
            this->devices_[this->last_zone_checked_index_]->generate_lgap_request(*&this->tx_buffer_);

            if (this->flow_control_pin_ != nullptr)
              this->flow_control_pin_->digital_write(true);

            // set correct request id and send over uart
            this->tx_buffer_[2] = this->last_request_id_;
            this->write_array(this->tx_buffer_.data(), this->tx_buffer_.size());
            this->flush();

            if (this->flow_control_pin_ != nullptr)
              this->flow_control_pin_->digital_write(false);

            // update state for last request
            this->last_request_zone_ = this->devices_[this->last_zone_checked_index_]->zone_number;
            this->last_send_time_ = this->last_zone_check_time_;
            this->last_receive_time_ = this->last_zone_check_time_;
            this->state_ = 1;
          }
        }
      }

      if (this->debug_ == true)
        ESP_LOGV(TAG, "Available: %d, State: %d", this->available(), this->state_);

      // 3. read the response (only read when expecting a response)
      while (this->available())
      {
        // handle reading timeouts
        if ((now - this->last_receive_time_) > this->receive_wait_time_)
        {
          ESP_LOGV(TAG, "Last receive time exceeded. Clearing buffer...");
          this->rx_buffer_.clear();
          this->state_ = 0;
          break;
        }

        // read byte and process
        uint8_t c;
        read_byte(&c);
        this->last_receive_time_ = now;
        ESP_LOGV(TAG, "LGAP received Byte  %d (0X%x)", c, c);

        // read the start of a new response
        if (c == 0x10 && this->state_ == 1 && this->rx_buffer_.size() == 0)
        {
          ESP_LOGV(TAG, "LGAP received start of new response");

          this->state_ = 2;
          this->rx_buffer_.clear();
          this->rx_buffer_.push_back(c);
          continue;
        }

        // process a valid byte
        if (this->state_ == 2)
        {
          ESP_LOGV(TAG, "State: 2");

          // add byte to rx buffer
          this->rx_buffer_.push_back(c);

          // valid climate responses are known to be 16 bytes long with the first byte being 0x10 (16), response length of 16 bytes and the last byte being the checksum
          if (this->rx_buffer_.size() == 16)
          {
            this->last_receive_time_ = now;

            // handle bad checksum
            if (calculate_checksum(this->rx_buffer_) != this->rx_buffer_[this->rx_buffer_.size() - 1])
            {
              // todo: include response bytes in printout
              ESP_LOGE(TAG, "Checksum failed for response");
              this->state_ = 0;
              break;
            }

            // TODO: add a flag to ignore out of order responses
            // check to see if the response is for the last request (request/response is in order)
            if (this->rx_buffer_[2] == this->last_request_id_ && this->rx_buffer_[4] == this->last_request_zone_)
            {
              // notify valid device components
              for (auto &device : this->devices_)
              {
                if (device->zone_number == this->rx_buffer_[4])
                {
                  device->on_message_received(this->rx_buffer_);
                }
              }
            }
            else
            {
              ESP_LOGV(TAG, "Response not for last request. Ignoring...");
            }

            // reset state
            this->state_ = 0;
            break;
          }
        }
      }

      // will overflow back to 0 when it reaches the top
      this->last_request_id_++;
    }
  } // namespace lgap
} // namespace esphome