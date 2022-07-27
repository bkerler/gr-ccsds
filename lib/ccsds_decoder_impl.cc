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
#include <volk/volk.h>
#include "ccsds_decoder_impl.h"
#include "ccsds.h"
#include "reed_solomon.h"

#define STATE_SYNC_SEARCH 0
#define STATE_CODEWORD 1

namespace gr {
  namespace ccsds {

    ccsds_decoder::sptr
    ccsds_decoder::make(int threshold, bool rs_decode, bool deinterleave, bool descramble, bool verbose, bool printing, int n_interleave, bool dual_basis)
    {
      return gnuradio::get_initial_sptr
        (new ccsds_decoder_impl(threshold, rs_decode, deinterleave, descramble, verbose, printing, n_interleave, dual_basis));
    }

    ccsds_decoder_impl::ccsds_decoder_impl(int threshold, bool rs_decode, bool deinterleave, bool descramble, bool verbose, bool printing, int n_interleave, bool dual_basis)
      : gr::sync_block("ccsds_decoder",
              gr::io_signature::make(1, 1, sizeof(uint8_t)),
              gr::io_signature::make(0, 0, 0)),
        d_threshold(threshold),
        d_rs_decode(rs_decode),
        d_deinterleave(deinterleave),
        d_descramble(descramble),
        d_verbose(verbose),
        d_printing(printing),
        d_n_interleave(n_interleave),
        d_dual_basis(dual_basis),
        d_num_frames_received(0),
        d_num_frames_decoded(0),
        d_num_subframes_decoded(0),
        d_num_fillframes_decoded(0)
    {
      message_port_register_out(pmt::mp("out"));

      for (uint8_t i=0; i<SYNC_WORD_LEN; i++) {
          d_sync_word = (d_sync_word << 8) | (SYNC_WORD[i] & 0xff);
      }

      /*
      // Create an alternative sync word that corresponds to a 90 deg
      //  constellation rotation. Others covered by differential encoding.
      //d_alt_sync_word = reverse_and_invert(d_sync_word, 2, 0x02);
      d_alt_sync_word = reverse_and_invert(d_sync_word, 2, 0x02, 32);
      if (d_verbose) printf("\tNormal sync word:\t%Zd\n", static_cast<uint64_t>(d_sync_word));
      if (d_verbose) printf("\tFormed an alternate sync word:\t%zd\n", static_cast<uint64_t>(d_alt_sync_word));
      d_alt_sync_state = false;
      */

      enter_sync_search();
    }

    ccsds_decoder_impl::~ccsds_decoder_impl()
    {
    }

    int
    ccsds_decoder_impl::work(int noutput_items,
        gr_vector_const_void_star &input_items,
        gr_vector_void_star &output_items)
    {
      const uint8_t *in = (const uint8_t *) input_items[0];

      uint16_t count = 0;
      while (count < noutput_items) {
          switch (d_decoder_state) {
              case STATE_SYNC_SEARCH:
                  // get next bit
                  d_data_reg = (d_data_reg << 1) | (in[count++] & 0x01);
                  if (compare_sync_word()) {
                      if (d_verbose) printf("\tsync word detected\n");
                      d_num_frames_received++;
                      d_alt_sync_state = false;
                      enter_codeword();
                  /*
                  } else if (compare_alt_sync_word()) {
                      if (d_verbose) printf("\talternate sync word detected %zd\n", static_cast<uint64_t>(reverse_and_invert(d_data_reg, 2, 0x02, 32)));
                      d_num_frames_received++;
                      d_alt_sync_state = true;
                      enter_codeword();
                  */
                  }
                  break;
              case STATE_CODEWORD:
                  // get next bit and pack then into full bytes
                  d_data_reg = (d_data_reg << 1) | (in[count++] & 0x01);
                  d_bit_counter++;
                  if (d_bit_counter == 8) {
                      /*
                      if (d_alt_sync_state) {
                          d_data_reg = reverse_and_invert(d_data_reg, 2, 0x02, 8) & 0xFF;
                      }
                      */
                      d_codeword[d_byte_counter] = d_data_reg;
                      d_byte_counter++;
                      d_bit_counter = 0;
                  }
                  // once the full codeword is loaded, try to decode the packet
                  if (d_byte_counter == codeword_len()) {
                      if (d_verbose) printf("\tloaded codeword of length %i\n", codeword_len());
                      if (d_printing) print_bytes(d_codeword, codeword_len());

                      bool success = decode_frame();
                      if (success) {
                          //if (is_fill_frame()) {
                          if (is_fill_frame_fast()) {
                              // detect and drop fill frames
                              d_num_fillframes_decoded++;

                              //return 0;
                          } else {
                              pmt::pmt_t pdu(pmt::cons(pmt::PMT_NIL, pmt::make_blob(d_payload, DATA_LEN)));
                              message_port_pub(pmt::mp("out"), pdu);
                          }
                      }

                      if (d_verbose) {
                          printf("\tframes received: %i\n\tframes decoded: %i\n\tsubframes decoded: %i\n\tfillframes decoded: %i\n",
                                  d_num_frames_received,
                                  d_num_frames_decoded,
                                  d_num_subframes_decoded,
                                  d_num_fillframes_decoded);
                      }
                      enter_sync_search();
                  }
                  break;
          }
      }
      return noutput_items;
    }

    void
    ccsds_decoder_impl::enter_sync_search()
    {
        if (d_verbose) printf("enter sync search\n");
        d_decoder_state = STATE_SYNC_SEARCH;
        d_data_reg = 0;
    }
    void
    ccsds_decoder_impl::enter_codeword()
    {
        if (d_verbose) printf("enter codeword\n");
        d_decoder_state = STATE_CODEWORD;
        d_byte_counter = 0;
        d_bit_counter = 0;
    }
    bool ccsds_decoder_impl::compare_sync_word()
    {
        uint32_t nwrong = 0;
        uint32_t wrong_bits = d_data_reg ^ d_sync_word;
        volk_32u_popcnt(&nwrong, wrong_bits);
        return nwrong <= d_threshold;
    }

    bool ccsds_decoder_impl::compare_alt_sync_word()
    {
        uint32_t nwrong = 0;
        uint32_t wrong_bits = d_data_reg ^ d_alt_sync_word;
        volk_32u_popcnt(&nwrong, wrong_bits);
        return nwrong <= d_threshold;
    }

    bool ccsds_decoder_impl::decode_frame()
    {
        // this will be set to false if a codeword is not decodable
        bool success = true;

        if (d_descramble) {
            descramble(d_codeword, codeword_len());
        }

        // deinterleave and decode rs blocks
        uint8_t rs_block[RS_BLOCK_LEN];
        int8_t nerrors;
        for (uint8_t i=0; i<d_n_interleave; i++) {
            for (uint8_t j=0; j<RS_BLOCK_LEN; j++) {

                if (d_deinterleave) {
                    rs_block[j] = d_codeword[i+(j*d_n_interleave)];
                } else {
                    rs_block[j] = d_codeword[i*RS_BLOCK_LEN + j];
                }

            }
            if (d_rs_decode) {
                nerrors = d_rs.decode(rs_block, d_dual_basis);
                if (nerrors == -1) {
                    if (d_verbose) printf("\tcould not decode rs block #%i\n", i);
                    success = false;
                } else {
                    if (d_verbose) printf("\tdecoded rs block #%i with %i errors\n", i, nerrors);
                    d_num_subframes_decoded++;
                }
            }
            if (d_deinterleave) {
                for (uint8_t j=0; j<RS_DATA_LEN; j++) {
                    d_payload[i+(j*d_n_interleave)] = rs_block[j];
                }
            } else {
                memcpy(&d_payload[i*RS_DATA_LEN], rs_block, RS_DATA_LEN);
            }
        }

        if (success) d_num_frames_decoded++;

        return success;
    }

    bool ccsds_decoder_impl::is_fill_frame()
    {
        uint16_t sum = 0;

        for (size_t i=0; i < DATA_LEN; i++) {
            sum += d_payload[i];
        }

        return (sum == 0);
    }

    bool ccsds_decoder_impl::is_fill_frame_fast()
    {
        return ((d_payload[0] == 0) && (d_payload[1] == 0));
    }

    uint8_t ccsds_decoder_impl::reverse(uint8_t x, uint8_t n)
    {
        // Bit-reverse every n bits
        uint8_t result = 0;
        for (uint8_t i=0; i<n; i++) {
            if ((x >> i) & 1)
                result |= 1 << (n - 1 - i);
        }

        return result;
    }

    uint8_t ccsds_decoder_impl::invert(uint8_t x, uint8_t mask)
    {
        // Invert the masked bits
        return x ^ mask;
    }

    uint32_t ccsds_decoder_impl::reverse_and_invert(uint32_t x, uint8_t n, uint8_t mask, uint8_t length)
    {
        uint32_t temp = 0;
        uint32_t result = 0;
        uint8_t sym = 0;

        for (uint8_t i=0; i<(length/n); i++) {
            sym = invert(reverse((x >> ((length-n) - n*i)) & 0x3, n), 0x02); // -Q I
            //sym = reverse((x >> ((length-n) - n*i)) & 0x3, n); // Q I
            //sym = invert((x >> ((length-n) - n*i)) & 0x3, 0x02); // Q -I

            //sym = reverse((x >> (30 - 2*i)) & 0x3, 2);
            //sym = invert(reverse((x >> (30 - 2*i)) & 0x3, 2), 0x01);
            temp = (temp << n) | (sym & 0xFF);
        }
        result = temp;

/*
        for (uint8_t i=(32/2); i>0; --i) {
            sym = invert(reverse((x >> (2*i)) & 0x3, 2), 0x02);
            temp = (temp >> 2) | (sym & 0xFF);
        }
        result = temp;
*/
        //for (uint8_t i=0; i<(32/2); i++) {
        //    sym = (temp >> (2*i)) & 0x3;
        //    result = (result << 2) | (sym & 0xFF);
        //}

        return result;
    }
  } /* namespace ccsds */
} /* namespace gr */
