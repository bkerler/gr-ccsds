/* -*- c++ -*- */
/*
 * Copyright 2016 André Løfaldli.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <gnuradio/io_signature.h>
#include "ccsds_encoder_impl.h"

#include "ccsds.h"
#include "reed_solomon.h"

namespace gr {
  namespace ccsds {

    ccsds_encoder::sptr
    ccsds_encoder::make(size_t itemsize, const std::string& len_tag_key, bool rs_encode, bool interleave, bool scramble, bool idle, float idle_block_time, bool asm_tail, bool printing, bool verbose, int n_interleave, bool dual_basis)
    {
      return gnuradio::get_initial_sptr
        (new ccsds_encoder_impl(itemsize, len_tag_key, rs_encode, interleave, scramble, idle, idle_block_time, asm_tail, printing, verbose, n_interleave, dual_basis));
    }

    /*
     * The private constructor
     */
    ccsds_encoder_impl::ccsds_encoder_impl(size_t itemsize, const std::string& len_tag_key, bool rs_encode, bool interleave, bool scramble, bool idle, float idle_block_time, bool asm_tail, bool printing, bool verbose, int n_interleave, bool dual_basis)
      : gr::tagged_stream_block("ccsds_encoder",
              gr::io_signature::make(itemsize==0 ? 0:1, itemsize==0 ? 0:1, itemsize),
              gr::io_signature::make(1, 1, sizeof(uint8_t)), len_tag_key),
        d_itemsize(itemsize),
        d_rs_encode(rs_encode),
        d_interleave(interleave),
        d_scramble(scramble),
        d_idle(idle),
        d_idle_block_time(idle_block_time),
        d_asm_tail(asm_tail),
        d_printing(printing),
        d_verbose(verbose),
        d_n_interleave(n_interleave),
        d_dual_basis(dual_basis),
        d_curr_len(0),
        d_num_frames(0),
        d_started(true)
    {
      if (d_itemsize == 0) {
          message_port_register_in(pmt::mp("in"));
      }

      if (d_asm_tail) {
        memcpy(d_first_pkt.sync_word, SYNC_WORD, SYNC_WORD_LEN);
        memcpy(d_first_pkt.post_sync_word, SYNC_WORD, SYNC_WORD_LEN);
        memcpy(d_asm_tail_pkt.sync_word, SYNC_WORD, SYNC_WORD_LEN);
      } else {
        memcpy(d_pkt.sync_word, SYNC_WORD, SYNC_WORD_LEN);
      }
    }

    /*
     * Our virtual destructor.
     */
    ccsds_encoder_impl::~ccsds_encoder_impl()
    {

    }

    void
    ccsds_encoder_impl::set_idle(bool idle)
    {
      d_idle = idle;
    }

    void
    ccsds_encoder_impl::set_idle_block_time(float idle_block_time)
    {
      d_idle_block_time = idle_block_time;
    }

    int
    ccsds_encoder_impl::calculate_output_stream_length(const gr_vector_int &ninput_items)
    {
        // copy from message queue
        if (d_itemsize == 0) {

            if (d_curr_len != 0) return 0;

            pmt::pmt_t msg(delete_head_nowait(pmt::mp("in")));
            if (msg.get() == NULL) {
                if (d_idle) {
                  // return an IDLE frame
                  d_curr_meta = pmt::make_dict();
                  d_curr_vec = pmt::make_u8vector(DATA_LEN, 0x00);
                  d_curr_len = pmt::length(d_curr_vec);

                  if (d_verbose) {
                      printf("Pushing an IDLE frame\n");
                  }
                } else {
                  return 0;
                }
                return TOTAL_FRAME_LEN;
            }
            if (!pmt::is_pair(msg)) {
                throw std::runtime_error("received a malformed pdu message");
            }
            d_curr_meta = pmt::car(msg);
            d_curr_vec = pmt::cdr(msg);
            d_curr_len = pmt::length(d_curr_vec);
        }

        if (!d_started && d_asm_tail) {
            return (total_frame_len() + SYNC_WORD_LEN);
        }
        return total_frame_len();
    }

    int
    ccsds_encoder_impl::work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {

      const uint8_t* in;
      if (d_itemsize == 0) {
          // see if there is anything to do
          // with filling, we should never get here
          if (d_curr_len == 0) return 0;

          if (d_curr_len != data_len()) {
              printf("[ERROR] expected %i bytes, got %i\n", data_len(), (int)d_curr_len);
              d_curr_len = 0;
              return 0;
          }

          size_t len = 0;
          in = (const uint8_t*) uniform_vector_elements(d_curr_vec, len);
      } else {
          in = (const uint8_t*) input_items[0];
      }
      uint8_t *out = (uint8_t *) output_items[0];
      //copy_stream_tags();

      uint8_t rs_block[RS_BLOCK_LEN];
      for (uint8_t i=0; i<d_n_interleave; i++) {

          // copy data from input to rs block
          if (d_interleave) {
              for (uint8_t j=0; j<RS_BLOCK_LEN; j++)
                  rs_block[j] = in[i + (d_n_interleave*j)];
          } else {
              memcpy(rs_block, &in[i*RS_DATA_LEN], RS_DATA_LEN);
          }

          // calculate parity data
          if (d_rs_encode) {
              d_rs.encode(rs_block, d_dual_basis);
          } else {
              memset(&rs_block[RS_DATA_LEN], 0, RS_PARITY_LEN);
          }

          if (!d_started && d_asm_tail) {
              // data into output array
              if (d_interleave) {
                  for (uint8_t j=0; j<RS_BLOCK_LEN; j++)
                      d_first_pkt.codeword[i + (RS_NBLOCKS*j)] = rs_block[j];
              } else {
                  memcpy(&d_first_pkt.codeword[i*RS_BLOCK_LEN], rs_block, RS_BLOCK_LEN);
              }
          } else if (d_asm_tail) {
              // data into output array
              if (d_interleave) {
                  for (uint8_t j=0; j<RS_BLOCK_LEN; j++)
                      d_asm_tail_pkt.codeword[i + (RS_NBLOCKS*j)] = rs_block[j];
              } else {
                  memcpy(&d_asm_tail_pkt.codeword[i*RS_BLOCK_LEN], rs_block, RS_BLOCK_LEN);
              }
          } else {
              // data into output array
              if (d_interleave) {
                  for (uint8_t j=0; j<RS_BLOCK_LEN; j++)
                      d_pkt.codeword[i + (RS_NBLOCKS*j)] = rs_block[j];
              } else {
                  memcpy(&d_pkt.codeword[i*RS_BLOCK_LEN], rs_block, RS_BLOCK_LEN);
              }
          }
      }

      if (d_scramble) {
          if (!d_started && d_asm_tail) {
              scramble(d_first_pkt.codeword, codeword_len());
          } else if (d_asm_tail) {
              scramble(d_asm_tail_pkt.codeword, codeword_len());
          } else {
              scramble(d_pkt.codeword, codeword_len());
          }
      }

      d_num_frames++;
      if (d_verbose) {
          printf("sending %i bytes of data\n", total_frame_len());
          printf("number of frames transmitted: %i\n", d_num_frames);
      }

      if (d_printing) {
          if (!d_started && d_asm_tail) {
              print_bytes(d_first_pkt.codeword, codeword_len());
          } else if (d_asm_tail) {
              print_bytes(d_asm_tail_pkt.codeword, codeword_len());
          } else {
              print_bytes(d_pkt.codeword, codeword_len());
          }
      }

      // copy data into output array
      if ((!d_started) && (d_asm_tail)) {
          // ASM + PACKET + ASM
          memcpy(out, &d_first_pkt, total_frame_len() + SYNC_WORD_LEN);
          d_started = true;
          //printf("\n\nFirst packet order\n\n");

          /*
          for (size_t i=0; i < (TOTAL_FRAME_LEN + SYNC_WORD_LEN); i++) {
              printf("%d ", out[i]);
          }
          printf("\n");
          */

          return (total_frame_len() + SYNC_WORD_LEN);
      } else if (d_asm_tail) {
          // PACKET + ASM
          memcpy(out, &d_asm_tail_pkt, total_frame_len());
          //printf("\n\nReverse packet order\n\n");
      } else {
          // ASM + PACKET
          memcpy(out, &d_pkt, total_frame_len());
          //printf("\n\nNormal packet order\n\n");
      }

      /*
      for (size_t i=0; i < (TOTAL_FRAME_LEN); i++) {
          printf("%d ", out[i]);
      }
      printf("\n");
      */

      // reset state
      d_curr_len = 0;
      return total_frame_len();
    }

    void
    ccsds_encoder_impl::copy_stream_tags()
    {
        if (!pmt::eq(d_curr_meta, pmt::PMT_NIL)) {
            pmt::pmt_t klist(pmt::dict_keys(d_curr_meta));
            for (size_t i=0; i<pmt::length(klist); i++) {
                pmt::pmt_t k(pmt::nth(i, klist));
                pmt::pmt_t v(pmt::dict_ref(d_curr_meta, k, pmt::PMT_NIL));
                add_item_tag(0, nitems_written(0), k, v, alias_pmt());
            }
        }
    }

  } /* namespace ccsds */
} /* namespace gr */
