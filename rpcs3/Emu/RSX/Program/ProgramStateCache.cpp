#include "stdafx.h"
#include "ProgramStateCache.h"
#include "Emu/system_config.h"

#include <stack>
#include "util/v128.hpp"

using namespace program_hash_util;

usz vertex_program_utils::get_vertex_program_ucode_hash(const RSXVertexProgram &program)
{
	// 64-bit Fowler/Noll/Vo FNV-1a hash code
	usz hash = 0xCBF29CE484222325ULL;
	const void* instbuffer = program.data.data();
	usz instIndex = 0;

	for (unsigned i = 0; i < program.data.size() / 4; i++)
	{
		if (program.instruction_mask[i])
		{
			const auto inst = v128::loadu(instbuffer, instIndex);
			hash ^= inst._u64[0];
			hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
			hash ^= inst._u64[1];
			hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		}

		instIndex++;
	}
	return hash;
}

vertex_program_utils::vertex_program_metadata vertex_program_utils::analyse_vertex_program(const u32* data, u32 entry, RSXVertexProgram& dst_prog)
{
	vertex_program_utils::vertex_program_metadata result{};
	//u32 last_instruction_address = 0;
	//u32 first_instruction_address = entry;

	std::bitset<rsx::max_vertex_program_instructions> instructions_to_patch;
	std::pair<u32, u32> instruction_range{ umax, 0 };
	bool has_branch_instruction = false;
	std::stack<u32> call_stack;

	D3  d3;
	D2  d2;
	D1  d1;
	D0  d0;

	std::function<void(u32, bool)> walk_function = [&](u32 start, bool fast_exit)
	{
		u32 current_instruction = start;
		std::set<u32> conditional_targets;
		bool has_printed_error = false;

		while (true)
		{
			ensure(current_instruction < rsx::max_vertex_program_instructions);

			if (result.instruction_mask[current_instruction])
			{
				if (!fast_exit)
				{
					if (!has_printed_error)
					{
						// This can be harmless if a dangling RET was encountered before
						rsx_log.error("vp_analyser: Possible infinite loop detected");
						has_printed_error = true;
					}
					current_instruction++;
					continue;
				}
				else
				{
					// Block walk, looking for earliest exit
					break;
				}
			}

			const auto instruction = v128::loadu(&data[current_instruction * 4]);
			d1.HEX = instruction._u32[1];
			d3.HEX = instruction._u32[3];

			// Touch current instruction
			result.instruction_mask[current_instruction] = true;
			instruction_range.first = std::min(current_instruction, instruction_range.first);
			instruction_range.second = std::max(current_instruction, instruction_range.second);

			// Whether to check if the current instruction references an input stream
			bool test_input_read = false;

			// Basic vec op analysis, must be done before flow analysis
			switch (d1.vec_opcode)
			{
			case RSX_VEC_OPCODE_TXL:
			{
				d2.HEX = instruction._u32[2];
				result.referenced_textures_mask |= (1 << d2.tex_num);
				break;
			}
			default:
			{
				test_input_read = !!d1.input_src;
				break;
			}
			}

			bool static_jump = false;
			bool function_call = true;

			switch (d1.sca_opcode)
			{
			case RSX_SCA_OPCODE_BRI:
			{
				d0.HEX = instruction._u32[0];
				static_jump = (d0.cond == 0x7);
				[[fallthrough]];
			}
			case RSX_SCA_OPCODE_BRB:
			{
				function_call = false;
				[[fallthrough]];
			}
			case RSX_SCA_OPCODE_CAL:
			case RSX_SCA_OPCODE_CLI:
			case RSX_SCA_OPCODE_CLB:
			{
				// Need to patch the jump address to be consistent wherever the program is located
				instructions_to_patch[current_instruction] = true;
				has_branch_instruction = true;

				d0.HEX = instruction._u32[0];
				d2.HEX = instruction._u32[2];
				const u32 jump_address = (d0.iaddrh2 << 9) | (d2.iaddrh << 3) | d3.iaddrl;

				if (function_call)
				{
					call_stack.push(current_instruction + 1);
					current_instruction = jump_address;
					continue;
				}
				else if (static_jump)
				{
					// NOTE: This will skip potential jump target blocks between current->target
					current_instruction = jump_address;
					continue;
				}
				else
				{
					// Set possible end address and proceed as usual
					conditional_targets.emplace(jump_address);
					instruction_range.second = std::max(jump_address, instruction_range.second);
				}

				break;
			}
			case RSX_SCA_OPCODE_RET:
			{
				if (call_stack.empty())
				{
					rsx_log.error("vp_analyser: RET found outside subroutine call");
				}
				else
				{
					current_instruction = call_stack.top();
					call_stack.pop();
					continue;
				}

				break;
			}
			default:
			{
				test_input_read = !!d1.input_src;
				break;
			}
			}

			if (test_input_read)
			{
				// Type is encoded in the first 2 bits of each block
				d2.HEX = instruction._u32[2];

				const auto src0 = d2.src0l;
				const auto src1 = d2.src1;
				const auto src2 = d3.src2l;

				if ((src0 | src1 | src2) & RSX_VP_REGISTER_TYPE_INPUT)
				{
					result.referenced_inputs_mask |= (1 << d1.input_src);
				}
			}

			if ((d3.end && (fast_exit || current_instruction >= instruction_range.second)) ||
				(current_instruction + 1) == rsx::max_vertex_program_instructions)
			{
				break;
			}

			current_instruction++;
		}

		for (const u32 target : conditional_targets)
		{
			if (!result.instruction_mask[target])
			{
				walk_function(target, true);
			}
		}
	};

	if (g_cfg.video.debug_program_analyser)
	{
		fs::file dump(fs::get_cache_dir() + "shaderlog/vp_analyser.bin", fs::rewrite);
		dump.write(&entry, 4);
		dump.write(data, rsx::max_vertex_program_instructions * 16);
		dump.close();
	}

	walk_function(entry, false);

	const u32 instruction_count = (instruction_range.second - instruction_range.first + 1);
	result.ucode_length = instruction_count * 16;

	dst_prog.base_address = instruction_range.first;
	dst_prog.entry = entry;
	dst_prog.data.resize(instruction_count * 4);
	dst_prog.instruction_mask = (result.instruction_mask >> instruction_range.first);

	if (!has_branch_instruction)
	{
		ensure(instruction_range.first == entry);
		std::memcpy(dst_prog.data.data(), data + (instruction_range.first * 4), result.ucode_length);
	}
	else
	{
		for (u32 i = instruction_range.first, count = 0; i <= instruction_range.second; ++i, ++count)
		{
			const u32* instruction = &data[i * 4];
			u32* dst = &dst_prog.data[count * 4];

			if (result.instruction_mask[i])
			{
				v128::storeu(v128::loadu(instruction), dst);

				if (instructions_to_patch[i])
				{
					d0.HEX = dst[0];
					d2.HEX = dst[2];
					d3.HEX = dst[3];

					u32 address = (d0.iaddrh2 << 9) | (d2.iaddrh << 3) | d3.iaddrl;
					address -= instruction_range.first;

					d0.iaddrh2 = (address >> 9) & 0x1;
					d2.iaddrh = (address >> 3) & 0x3F;
					d3.iaddrl = (address & 0x7);
					dst[0] = d0.HEX;
					dst[2] = d2.HEX;
					dst[3] = d3.HEX;

					dst_prog.jump_table.emplace(address);
				}
			}
			else
			{
				v128::storeu({}, dst);
			}
		}

		// Verification
		for (const u32 target : dst_prog.jump_table)
		{
			if (!dst_prog.instruction_mask[target])
			{
				rsx_log.error("vp_analyser: Failed, branch target 0x%x was not resolved", target);
			}
		}
	}

	result.referenced_inputs_mask |= 1u; // VPOS is always enabled, else no rendering can happen
	return result;
}

usz vertex_program_storage_hash::operator()(const RSXVertexProgram &program) const
{
	usz hash = vertex_program_utils::get_vertex_program_ucode_hash(program);
	hash ^= program.output_mask;
	hash ^= program.texture_state.texture_dimensions;
	return hash;
}

bool vertex_program_compare::operator()(const RSXVertexProgram &binary1, const RSXVertexProgram &binary2) const
{
	if (binary1.output_mask != binary2.output_mask)
		return false;
	if (binary1.texture_state != binary2.texture_state)
		return false;
	if (binary1.data.size() != binary2.data.size())
		return false;
	if (binary1.jump_table != binary2.jump_table)
		return false;

	const void* instBuffer1 = binary1.data.data();
	const void* instBuffer2 = binary2.data.data();
	usz instIndex = 0;
	for (unsigned i = 0; i < binary1.data.size() / 4; i++)
	{
		const auto active = binary1.instruction_mask[instIndex];
		if (active != binary2.instruction_mask[instIndex])
		{
			return false;
		}

		if (active)
		{
			const auto inst1 = v128::loadu(instBuffer1, instIndex);
			const auto inst2 = v128::loadu(instBuffer2, instIndex);
			if (inst1._u ^ inst2._u)
			{
				return false;
			}
		}

		instIndex++;
	}

	return true;
}


bool fragment_program_utils::is_constant(u32 sourceOperand)
{
	return ((sourceOperand >> 8) & 0x3) == 2;
}

usz fragment_program_utils::get_fragment_program_ucode_size(const void* ptr)
{
	const auto instBuffer = ptr;
	usz instIndex = 0;
	while (true)
	{
		const v128 inst = v128::loadu(instBuffer, instIndex);
		bool isSRC0Constant = is_constant(inst._u32[1]);
		bool isSRC1Constant = is_constant(inst._u32[2]);
		bool isSRC2Constant = is_constant(inst._u32[3]);
		bool end = (inst._u32[0] >> 8) & 0x1;

		if (isSRC0Constant || isSRC1Constant || isSRC2Constant)
		{
			instIndex += 2;
			if (end)
				return instIndex * 4 * 4;
			continue;
		}
		instIndex++;
		if (end)
			return (instIndex)* 4 * 4;
	}
}

fragment_program_utils::fragment_program_metadata fragment_program_utils::analyse_fragment_program(const void* ptr)
{
	fragment_program_utils::fragment_program_metadata result{};
	result.program_start_offset = -1;
	const auto instBuffer = ptr;
	s32 index = 0;

	while (true)
	{
		const auto inst = v128::loadu(instBuffer, index);

		// Check for opcode high bit which indicates a branch instructions (opcode 0x40...0x45)
		if (inst._u32[2] & (1 << 23))
		{
			// NOTE: Jump instructions are not yet proved to work outside of loops and if/else blocks
			// Otherwise we would need to follow the execution chain
			result.has_branch_instructions = true;
		}
		else
		{
			const u32 opcode = (inst._u32[0] >> 16) & 0x3F;
			if (opcode)
			{
				if (result.program_start_offset == umax)
				{
					result.program_start_offset = index * 16;
				}

				switch (opcode)
				{
				case RSX_FP_OPCODE_TEX:
				case RSX_FP_OPCODE_TEXBEM:
				case RSX_FP_OPCODE_TXP:
				case RSX_FP_OPCODE_TXPBEM:
				case RSX_FP_OPCODE_TXD:
				case RSX_FP_OPCODE_TXB:
				case RSX_FP_OPCODE_TXL:
				{
					//Bits 17-20 of word 1, swapped within u16 sections
					//Bits 16-23 are swapped into the upper 8 bits (24-31)
					const u32 tex_num = (inst._u32[0] >> 25) & 15;
					result.referenced_textures_mask |= (1 << tex_num);
					break;
				}
				case RSX_FP_OPCODE_PK4:
				case RSX_FP_OPCODE_UP4:
				case RSX_FP_OPCODE_PK2:
				case RSX_FP_OPCODE_UP2:
				case RSX_FP_OPCODE_PKB:
				case RSX_FP_OPCODE_UPB:
				case RSX_FP_OPCODE_PK16:
				case RSX_FP_OPCODE_UP16:
				case RSX_FP_OPCODE_PKG:
				case RSX_FP_OPCODE_UPG:
				{
					result.has_pack_instructions = true;
					break;
				}
				}
			}

			if (is_constant(inst._u32[1]) || is_constant(inst._u32[2]) || is_constant(inst._u32[3]))
			{
				//Instruction references constant, skip one slot occupied by data
				index++;
				result.program_ucode_length += 16;
				result.program_constants_buffer_length += 16;
			}
		}

		if (result.program_start_offset != umax)
		{
			result.program_ucode_length += 16;
		}

		if ((inst._u32[0] >> 8) & 0x1)
		{
			if (result.program_start_offset == umax)
			{
				result.program_start_offset = index * 16;
				result.program_ucode_length = 16;
				result.is_nop_shader = true;
			}

			break;
		}

		index++;
	}

	return result;
}

usz fragment_program_utils::get_fragment_program_ucode_hash(const RSXFragmentProgram& program)
{
	// 64-bit Fowler/Noll/Vo FNV-1a hash code
	usz hash = 0xCBF29CE484222325ULL;
	const void* instbuffer = program.get_data();
	usz instIndex = 0;
	while (true)
	{
		const auto inst = v128::loadu(instbuffer, instIndex);
		hash ^= inst._u64[0];
		hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		hash ^= inst._u64[1];
		hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		instIndex++;
		// Skip constants
		if (fragment_program_utils::is_constant(inst._u32[1]) ||
			fragment_program_utils::is_constant(inst._u32[2]) ||
			fragment_program_utils::is_constant(inst._u32[3]))
			instIndex++;

		bool end = (inst._u32[0] >> 8) & 0x1;
		if (end)
			return hash;
	}
	return 0;
}

usz fragment_program_storage_hash::operator()(const RSXFragmentProgram& program) const
{
	usz hash = fragment_program_utils::get_fragment_program_ucode_hash(program);
	hash ^= program.ctrl;
	hash ^= +program.two_sided_lighting;
	hash ^= program.texture_state.texture_dimensions;
	hash ^= program.texture_state.shadow_textures;
	hash ^= program.texture_state.redirected_textures;
	hash ^= program.texcoord_control_mask;

	return hash;
}

bool fragment_program_compare::operator()(const RSXFragmentProgram& binary1, const RSXFragmentProgram& binary2) const
{
	if (binary1.ctrl != binary2.ctrl || binary1.texture_state != binary2.texture_state ||
		binary1.texcoord_control_mask != binary2.texcoord_control_mask ||
		binary1.two_sided_lighting != binary2.two_sided_lighting)
		return false;

	const void* instBuffer1 = binary1.get_data();
	const void* instBuffer2 = binary2.get_data();
	usz instIndex = 0;
	while (true)
	{
		const auto inst1 = v128::loadu(instBuffer1, instIndex);
		const auto inst2 = v128::loadu(instBuffer2, instIndex);

		if (inst1._u ^ inst2._u)
			return false;

		instIndex++;
		// Skip constants
		if (fragment_program_utils::is_constant(inst1._u32[1]) ||
			fragment_program_utils::is_constant(inst1._u32[2]) ||
			fragment_program_utils::is_constant(inst1._u32[3]))
			instIndex++;

		bool end = ((inst1._u32[0] >> 8) & 0x1) && ((inst2._u32[0] >> 8) & 0x1);
		if (end)
			return true;
	}
}
