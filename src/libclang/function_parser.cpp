// Copyright (C) 2017 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include <cppast/cpp_function.hpp>
#include <cppast/cpp_member_function.hpp>

#include "libclang_visitor.hpp"
#include "parse_functions.hpp"

using namespace cppast;

namespace
{
    std::unique_ptr<cpp_function_parameter> parse_parameter(const detail::parse_context& context,
                                                            const CXCursor&              cur)
    {
        auto name = detail::get_cursor_name(cur);
        auto type = detail::parse_type(context, clang_getCursorType(cur));

        std::unique_ptr<cpp_expression> default_value;
        detail::visit_children(cur, [&](const CXCursor& child) {
            DEBUG_ASSERT(clang_isExpression(child.kind) && !default_value,
                         detail::parse_error_handler{}, child,
                         "unexpected child cursor of function parameter");
            default_value = detail::parse_expression(context, child);
        });

        return cpp_function_parameter::build(*context.idx, detail::get_entity_id(cur), name.c_str(),
                                             std::move(type), std::move(default_value));
    }

    template <class Builder>
    void add_parameters(const detail::parse_context& context, Builder& builder, const CXCursor& cur)
    {
        detail::visit_children(cur, [&](const CXCursor& child) {
            if (clang_getCursorKind(child) != CXCursor_ParmDecl)
                return;

            try
            {
                auto parameter = parse_parameter(context, child);
                builder.add_parameter(std::move(parameter));
            }
            catch (detail::parse_error& ex)
            {
                context.logger->log("libclang parser", ex.get_diagnostic());
            }
        });
    }

    void skip_parameters(detail::token_stream& stream)
    {
        detail::skip_brackets(stream);
    }

    // just the tokens occurring in the prefix
    struct prefix_info
    {
        bool is_constexpr = false;
        bool is_virtual   = false;
    };

    prefix_info parse_prefix_info(detail::token_stream& stream, const detail::cxstring& name)
    {
        prefix_info result;

        // just check for keywords until we've reached the function name
        // notes: name can have multiple tokens if it is an operator
        while (!detail::skip_if(stream, name.c_str(), true))
        {
            if (detail::skip_if(stream, "constexpr"))
                result.is_constexpr = true;
            else if (detail::skip_if(stream, "virtual"))
                result.is_virtual = true;
            else
                stream.bump();
        }

        return result;
    }

    // just the tokens occurring in the suffix
    struct suffix_info
    {
        std::unique_ptr<cpp_expression> noexcept_condition;
        cpp_function_body_kind          body_kind;
        cpp_cv                          cv_qualifier  = cpp_cv_none;
        cpp_reference                   ref_qualifier = cpp_ref_none;
        cpp_virtual                     virtual_keywords;

        suffix_info(const CXCursor& cur)
        : body_kind(clang_isCursorDefinition(cur) ? cpp_function_definition :
                                                    cpp_function_declaration)
        {
        }
    };

    cpp_cv parse_cv(detail::token_stream& stream)
    {
        if (detail::skip_if(stream, "const"))
        {
            if (detail::skip_if(stream, "volatile"))
                return cpp_cv_const_volatile;
            else
                return cpp_cv_const;
        }
        else if (detail::skip_if(stream, "volatile"))
        {
            if (detail::skip_if(stream, "const"))
                return cpp_cv_const_volatile;
            else
                return cpp_cv_volatile;
        }
        else
            return cpp_cv_none;
    }

    cpp_reference parse_ref(detail::token_stream& stream)
    {
        if (detail::skip_if(stream, "&"))
            return cpp_ref_lvalue;
        else if (detail::skip_if(stream, "&&"))
            return cpp_ref_rvalue;
        else
            return cpp_ref_none;
    }

    std::unique_ptr<cpp_expression> parse_noexcept(detail::token_stream&        stream,
                                                   const detail::parse_context& context)
    {
        if (!detail::skip_if(stream, "noexcept"))
            return nullptr;

        auto type = cpp_builtin_type::build("bool");
        if (stream.peek().value() != "(")
            return cpp_literal_expression::build(std::move(type), "true");

        auto closing = detail::find_closing_bracket(stream);

        detail::skip(stream, "(");
        auto expr = detail::parse_raw_expression(context, stream, closing, std::move(type));
        detail::skip(stream, ")");

        return expr;
    }

    cpp_function_body_kind parse_body_kind(detail::token_stream& stream, bool& pure_virtual)
    {
        pure_virtual = false;
        if (detail::skip_if(stream, "default"))
            return cpp_function_defaulted;
        else if (detail::skip_if(stream, "delete"))
            return cpp_function_deleted;
        else if (detail::skip_if(stream, "0"))
        {
            pure_virtual = true;
            return cpp_function_declaration;
        }

        DEBUG_UNREACHABLE(detail::parse_error_handler{}, stream.cursor(),
                          "unexpected token for function body kind");
        return cpp_function_declaration;
    }

    void parse_body(detail::token_stream& stream, suffix_info& result)
    {
        auto pure_virtual = false;
        result.body_kind  = parse_body_kind(stream, pure_virtual);
        if (pure_virtual)
        {
            if (result.virtual_keywords)
                result.virtual_keywords.value() &= cpp_virtual_flags::pure;
            else
                result.virtual_keywords = cpp_virtual_flags::pure;
        }
    }

    // precondition: we've skipped the function parameters
    suffix_info parse_suffix_info(detail::token_stream&        stream,
                                  const detail::parse_context& context)
    {
        suffix_info result(stream.cursor());

        // syntax: <attribute> <cv> <ref> <exception>
        detail::skip_attribute(stream);
        result.cv_qualifier  = parse_cv(stream);
        result.ref_qualifier = parse_ref(stream);
        if (detail::skip_if(stream, "throw"))
            // just because I can
            detail::skip_brackets(stream);
        result.noexcept_condition = parse_noexcept(stream, context);

        // check if we have leftovers of the return type
        // i.e.: `void (*foo(int a, int b) const)(int)`;
        //                                ^^^^^^- attributes
        //                                      ^^^^^^- leftovers
        // if we have a closing parenthesis, skip brackets
        if (detail::skip_if(stream, ")"))
            detail::skip_brackets(stream);

        // check for trailing return type
        if (detail::skip_if(stream, "->"))
        {
            // this is rather tricky to skip
            // so loop over all tokens and see if matching keytokens occur
            // note that this isn't quite correct
            // use a heuristic to skip brackets, which should be good enough
            while (!stream.done())
            {
                if (stream.peek() == "(" || stream.peek() == "[" || stream.peek() == "<"
                    || stream.peek() == "{")
                    detail::skip_brackets(stream);
                else if (detail::skip_if(stream, "override"))
                {
                    if (result.virtual_keywords)
                        result.virtual_keywords.value() |= cpp_virtual_flags::override;
                    else
                        result.virtual_keywords = cpp_virtual_flags::override;
                }
                else if (detail::skip_if(stream, "final"))
                {
                    if (result.virtual_keywords)
                        result.virtual_keywords.value() |= cpp_virtual_flags::final;
                    else
                        result.virtual_keywords = cpp_virtual_flags::final;
                }
                else if (detail::skip_if(stream, "="))
                    parse_body(stream, result);
                else
                    stream.bump();
            }
        }
        else
        {
            // syntax: <virtuals> <body>
            if (detail::skip_if(stream, "override"))
            {
                result.virtual_keywords = cpp_virtual_flags::override;
                if (detail::skip_if(stream, "final"))
                    result.virtual_keywords.value() |= cpp_virtual_flags::final;
            }
            else if (detail::skip_if(stream, "final"))
            {
                result.virtual_keywords = cpp_virtual_flags::final;
                if (detail::skip_if(stream, "override"))
                    result.virtual_keywords.value() |= cpp_virtual_flags::override;
            }

            if (detail::skip_if(stream, "="))
                parse_body(stream, result);
        }

        return result;
    }

    std::unique_ptr<cpp_entity> parse_cpp_function_impl(const detail::parse_context& context,
                                                        const CXCursor&              cur)
    {
        auto name = detail::get_cursor_name(cur);

        cpp_function::builder builder(name.c_str(),
                                      detail::parse_type(context, clang_getCursorResultType(cur)));
        add_parameters(context, builder, cur);
        if (clang_Cursor_isVariadic(cur))
            builder.is_variadic();
        builder.storage_class(detail::get_storage_class(cur));

        detail::tokenizer    tokenizer(context.tu, context.file, cur);
        detail::token_stream stream(tokenizer, cur);

        auto prefix = parse_prefix_info(stream, name);
        DEBUG_ASSERT(!prefix.is_virtual, detail::parse_error_handler{}, cur,
                     "free function cannot be virtual");
        if (prefix.is_constexpr)
            builder.is_constexpr();

        skip_parameters(stream);

        auto suffix = parse_suffix_info(stream, context);
        DEBUG_ASSERT(suffix.cv_qualifier == cpp_cv_none && suffix.ref_qualifier == cpp_ref_none
                         && !suffix.virtual_keywords,
                     detail::parse_error_handler{}, cur, "unexpected tokens in function suffix");
        if (suffix.noexcept_condition)
            builder.noexcept_condition(std::move(suffix.noexcept_condition));

        return builder.finish(*context.idx, detail::get_entity_id(cur), suffix.body_kind);
    }
}

std::unique_ptr<cpp_entity> detail::parse_cpp_function(const detail::parse_context& context,
                                                       const CXCursor&              cur)
{
    DEBUG_ASSERT(clang_getCursorKind(cur) == CXCursor_FunctionDecl, detail::assert_handler{});
    return parse_cpp_function_impl(context, cur);
}

std::unique_ptr<cpp_entity> detail::try_parse_static_cpp_function(
    const detail::parse_context& context, const CXCursor& cur)
{
    DEBUG_ASSERT(clang_getCursorKind(cur) == CXCursor_CXXMethod, detail::assert_handler{});
    if (clang_CXXMethod_isStatic(cur))
        return parse_cpp_function_impl(context, cur);
    return nullptr;
}

namespace
{
    bool overrides_function(const CXCursor& cur)
    {
        CXCursor* overrides = nullptr;
        auto      num       = 0u;
        clang_getOverriddenCursors(cur, &overrides, &num);
        clang_disposeOverriddenCursors(overrides);
        return num != 0u;
    }

    cpp_virtual calculate_virtual(const CXCursor& cur, bool virtual_keyword,
                                  const cpp_virtual& virtual_suffix)
    {
        if (!clang_CXXMethod_isVirtual(cur))
        {
            // not a virtual function, ensure it wasn't parsed that way
            DEBUG_ASSERT(!virtual_keyword && !virtual_suffix.has_value(),
                         detail::parse_error_handler{}, cur, "virtualness not parsed properly");
            return {};
        }
        else if (clang_CXXMethod_isPureVirtual(cur))
        {
            // pure virtual function - all information in the suffix
            DEBUG_ASSERT(virtual_suffix.has_value()
                             && virtual_suffix.value() & cpp_virtual_flags::pure,
                         detail::parse_error_handler{}, cur, "pure virtual not detected");
            return virtual_suffix;
        }
        else
        {
            // non-pure virtual function
            DEBUG_ASSERT(!virtual_suffix.has_value()
                             || !(virtual_suffix.value() & cpp_virtual_flags::pure),
                         detail::parse_error_handler{}, cur,
                         "pure virtual function detected, even though it isn't");
            // calculate whether it overrides
            auto overrides = !virtual_keyword
                             || (virtual_suffix.has_value()
                                 && virtual_suffix.value() & cpp_virtual_flags::override)
                             || overrides_function(cur);

            // result are all the flags in the suffix
            auto result = virtual_suffix;
            if (!result)
                // make sure it isn't empty
                result.emplace();
            if (overrides)
                // make sure it contains the override flag
                result.value() |= cpp_virtual_flags::override;
            return result;
        }
    }

    template <class Builder>
    std::unique_ptr<cpp_entity> handle_suffix(const detail::parse_context& context,
                                              const CXCursor& cur, Builder& builder,
                                              detail::token_stream& stream, bool is_virtual)
    {
        auto suffix = parse_suffix_info(stream, context);
        builder.cv_ref_qualifier(suffix.cv_qualifier, suffix.ref_qualifier);
        if (suffix.noexcept_condition)
            builder.noexcept_condition(move(suffix.noexcept_condition));
        if (auto virt = calculate_virtual(cur, is_virtual, suffix.virtual_keywords))
            builder.virtual_info(virt.value());

        return builder.finish(*context.idx, detail::get_entity_id(cur), suffix.body_kind);
    }
}

std::unique_ptr<cpp_entity> detail::parse_cpp_member_function(const detail::parse_context& context,
                                                              const CXCursor&              cur)
{
    DEBUG_ASSERT(clang_getCursorKind(cur) == CXCursor_CXXMethod, detail::assert_handler{});
    auto name = detail::get_cursor_name(cur);

    cpp_member_function::builder builder(name.c_str(),
                                         detail::parse_type(context,
                                                            clang_getCursorResultType(cur)));
    add_parameters(context, builder, cur);
    if (clang_Cursor_isVariadic(cur))
        builder.is_variadic();

    detail::tokenizer    tokenizer(context.tu, context.file, cur);
    detail::token_stream stream(tokenizer, cur);

    auto prefix = parse_prefix_info(stream, name);
    if (prefix.is_constexpr)
        builder.is_constexpr();

    skip_parameters(stream);
    return handle_suffix(context, cur, builder, stream, prefix.is_virtual);
}

std::unique_ptr<cpp_entity> detail::parse_cpp_conversion_op(const detail::parse_context& context,
                                                            const CXCursor&              cur)
{
    DEBUG_ASSERT(clang_getCursorKind(cur) == CXCursor_ConversionFunction, detail::assert_handler{});
    cpp_conversion_op::builder builder(detail::parse_type(context, clang_getCursorResultType(cur)));

    detail::tokenizer    tokenizer(context.tu, context.file, cur);
    detail::token_stream stream(tokenizer, cur);

    // look for constexpr, explicit, virtual
    // must come before the operator token
    auto is_virtual = false;
    while (!detail::skip_if(stream, "operator"))
    {
        if (detail::skip_if(stream, "virtual"))
            is_virtual = true;
        else if (detail::skip_if(stream, "constexpr"))
            builder.is_constexpr();
        else if (detail::skip_if(stream, "explicit"))
            builder.is_explicit();
        else
            stream.bump();
    }

    // heuristic to find arguments tokens
    // skip forward, skipping inside brackets
    while (true)
    {
        if (detail::skip_if(stream, "("))
        {
            if (detail::skip_if(stream, ")"))
                break;
            else
                detail::skip_brackets(stream);
        }
        else if (detail::skip_if(stream, "["))
            detail::skip_brackets(stream);
        else if (detail::skip_if(stream, "{"))
            detail::skip_brackets(stream);
        else if (detail::skip_if(stream, "<"))
            detail::skip_brackets(stream);
        else
            stream.bump();
    }

    return handle_suffix(context, cur, builder, stream, is_virtual);
}