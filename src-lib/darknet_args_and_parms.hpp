/* Darknet/YOLO:  https://github.com/hank-ai/darknet
 * Copyright 2024-2025 Stephane Charette
 */

#pragma once

#include "darknet.hpp"


namespace Darknet
{
	class ArgsAndParms final
	{
		public:

			enum class EType
			{
				kInvalid,
				kCommand,
				kFunction,
				kParameter
			};

			/// Destructor.
			~ArgsAndParms();

			/// Default constructor is needed for std::map.
			ArgsAndParms();

			/** Constructor.
			 * @param [in] n1 is the argument name.
			 * @param [in] n2 is an alternate name or spelling.  This may be blank if there are no alternate spellings.
			 * @param [in] txt is a short text description for this parameter.
			 */
			ArgsAndParms(const std::string & n1, const std::string & n2 = "", const std::string & txt = "");

			/** Constructor.
			 * @param [in] n1 is the command or function name.
			 * @param [in] t sets the parameter type.
			 * @param [in] txt is a short text description for this parameter.
			 */
			ArgsAndParms(const std::string & n1, const EType t, const std::string & txt = "");

			/** Constructor.
			 * @param [in] n1 is the command or function name.
			 * @param [in] n2 is an alternate name or spelling.
			 * @param [in] t sets the parameter type.
			 * @param [in] txt is a short text description for this parameter.
			 */
			ArgsAndParms(const std::string & n1, const std::string & n2, const EType t, const std::string & txt);

			/** Constructor.  This parameter requires the next argument be an @p int parameter.
			 * @param [in] n1 is the argument name.
			 * @param [in] n2 is an alternate name or spelling.  This may be blank if there are no alternate spellings.
			 * @param [in] i is the default value to use for this parameter.
			 * @param [in] txt is a short text description for this parameter.
			 */
			ArgsAndParms(const std::string & n1, const std::string & n2, const int i, const std::string & txt = "");

			/** Constructor.  This parameter requires the next argument be a @p float parameter.
			 * @param [in] n1 is the argument name.
			 * @param [in] n2 is an alternate name or spelling.  This may be blank if there are no alternate spellings.
			 * @param [in] f is the default value to use for this parameter.
			 * @param [in] txt is a short text description for this parameter.
			 */
			ArgsAndParms(const std::string & n1, const std::string & n2, const float f, const std::string & txt = "");

			/** Constructor.  This parameter requires the next argument be a @p string parameter.
			 * @param [in] n1 is the argument name.
			 * @param [in] n2 is an alternate name or spelling.  This may be blank if there are no alternate spellings.
			 * @param [in] str is the default value to use for this parameter.
			 * @param [in] txt is a short text description for this parameter.
			 */
			ArgsAndParms(const std::string & n1, const std::string & n2, const std::string & str, const std::string & txt);

			/// The name of the argument or command.  For example, this could be @p "dontshow" or @p "version".
			std::string name;

			/// If the argument or command has an alternate spelling.  For example, this could be @p "color" (vs @p "colour").
			std::string name_alternate;

			std::string description;

			EType type;

			/// If an additional parameter is expected.  For example, @p "--threshold" should be followed by a number.
			bool expect_parm;

			/// The argument index into argv[].
			int arg_index;

			/// If @p expect_parm is @p true, then this would be the numeric value that comes next.
			float value;

			/// If @p expect_parm is @p true, then this would be the text string that comes next.
			std::string str;

			/// If this parameter is a filename, or the value is a filename, the path is stored here.
			std::filesystem::path filename;

			/// Needed to store these objects in an ordered set.
			bool operator<(const ArgsAndParms & rhs) const
			{
				// Multiple copies of the same name can exist.  For example, "detector map" command exists
				// at the same time as the "-map" parameter for "detector train".  So when we compare the keys,
				// we need to take into consideration the type as well as the name.
				//
				// IMPORTANT TO NOTE:  This means we cannot use find() to locate a command.  We need to iterate
				// over the entire container.
				//
				const auto str1 = name		+ std::to_string(static_cast<int>(type		));
				const auto str2 = rhs.name	+ std::to_string(static_cast<int>(rhs.type	));

				return str1 < str2;
			}
	};

	/// Meant to be used only for debug purposes.
	std::ostream & operator<<(std::ostream & os, const Darknet::ArgsAndParms & rhs);

	using SArgsAndParms = std::set<ArgsAndParms>;

	/// The key is the argument name, the value is the details for that argument.
	using MArgsAndParms = std::map<std::string, ArgsAndParms>;

	/** Get all the possible arguments used by Darknet.  This is not what the user specified to @p main() but @em all the
	 * possible arguments against which we validate the input.
	 */
	const SArgsAndParms & get_all_possible_arguments();

	void display_usage();
}
